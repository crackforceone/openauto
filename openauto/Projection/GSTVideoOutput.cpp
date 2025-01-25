/*
*  This file is part of openauto project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  openauto is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  openauto is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with openauto. If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef USE_GST
#include "aasdk/Common/Data.hpp"
#include "openauto/Projection/GSTVideoOutput.hpp"
#include "OpenautoLog.hpp"
#include "h264_stream.h"
#include <QTimer>
// these are needed only for pretty printing of data, to be removed
#include <sstream>
#include <string>
#include <iostream>
#include <iomanip>

#include <QGuiApplication>
#include <QScreen>


namespace openauto
{
namespace projection
{

GSTVideoOutput::GSTVideoOutput(configuration::IConfiguration::Pointer configuration, QWidget* videoContainer, std::function<void(bool)> activeCallback)
    : VideoOutput(std::move(configuration))
    , videoContainer_(videoContainer)
    , activeCallback_(activeCallback)
{
    
    this->moveToThread(QApplication::instance()->thread());
    videoWidget_ = new QQuickWidget(videoContainer_);
    videoWidget_->setClearColor(QColor(18, 18, 18));
    videoWidget_->installEventFilter(this);

    GError* error = nullptr;


    
    //QSize screenSize = QGuiApplication::primaryScreen()->size();
    //gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink),
    //screenSize.width()-videoContainer_->width(),
    //screenSize.height()-videoContainer_->height(),
    //videoContainer_->width(),
    //videoContainer_->height());

    std::string vidLaunchStr = "appsrc name=mysrc is-live=true block=true max-latency=100 do-timestamp=false stream-type=stream ! queue ! h264parse ! capssetter caps=\"video/x-h264\" ! ";
    vidLaunchStr += ToPipeline(findPreferredVideoDecoder());
    vidLaunchStr += " ! videocrop top=0 bottom=0 name=videocropper ! capsfilter caps=video/x-raw name=mycapsfilter";
    
    
    vidPipeline_ = gst_parse_launch(vidLaunchStr.c_str(), &error);
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(vidPipeline_));
    gst_bus_add_watch(bus, (GstBusFunc)&GSTVideoOutput::busCallback, this);
    gst_object_unref(bus);


    GstElement* capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    gst_bin_add(GST_BIN(vidPipeline_), kmssink);
    gst_element_link(capsFilter, kmssink);
    vidSrc_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(vidPipeline_), "mysrc"));
    gst_app_src_set_stream_type(vidSrc_, GST_APP_STREAM_TYPE_STREAM);
    vidCrop_ = GST_VIDEO_FILTER(gst_bin_get_by_name(GST_BIN(vidPipeline_), "videocropper"));
    
    
    gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);
    checkVideoWidgetVisibility();
    
    connect(this, &GSTVideoOutput::startPlayback, this, &GSTVideoOutput::onStartPlayback, Qt::QueuedConnection);
    connect(this, &GSTVideoOutput::stopPlayback, this, &GSTVideoOutput::onStopPlayback, Qt::QueuedConnection);
}

GSTVideoOutput::~GSTVideoOutput()
{
    gst_object_unref(vidPipeline_);
    gst_object_unref(vidSrc_);
}

void GSTVideoOutput::checkVideoWidgetVisibility()
{
   bool isVisible = videoWidget_->isVisible();
   OPENAUTO_LOG(info) << "[GSTVideoOutput] VideoWidget visibility status: " << (isVisible ? "visible" : "not visible");
   
   GstElement* kmssink = gst_bin_get_by_name(GST_BIN(vidPipeline_), "kmssink");
   QSize screenSize = QGuiApplication::primaryScreen()->size();
   
   if(isVisible)
   {
       gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink), 
           screenSize.width()-videoContainer_->width(), 
           screenSize.height()-videoContainer_->height(), 
           videoContainer_->width(), 
           videoContainer_->height());
       OPENAUTO_LOG(info) << "[GSTVideoOutput] Show kmssink";
   }
   else
   {
        //This is just shoving the videosink to the side if the videoWidget_ is not visible.
        //Kind of a crackhead solution but it works and it's fast.
       gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink), 
           screenSize.width()*2, 
           0, 
           videoContainer_->width(), 
           videoContainer_->height());
       OPENAUTO_LOG(info) << "[GSTVideoOutput] Hide kmssink";
   }
}

bool GSTVideoOutput::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == videoWidget_) {
        switch (event->type()) {
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::WindowStateChange: {
                bool isVisible = videoWidget_->isVisible();
                if (isVisible != wasVisible) {
                    checkVideoWidgetVisibility();
                    wasVisible = isVisible;
                }
                break;
            }
            default:
                break;
        }
    }
    return QObject::eventFilter(obj, event);
}


H264_Decoder GSTVideoOutput::findPreferredVideoDecoder()
{
    for (H264_Decoder decoder : H264_Decoder_Priority_List) {
        GstElementFactory *decoder_factory = gst_element_factory_find (ToString(decoder));
        if(decoder_factory != nullptr){
            gst_object_unref(decoder_factory);
            OPENAUTO_LOG(info) << "[GSTVideoOutput] Selecting the " << ToString(decoder) << " h264 decoder";
            return decoder;
        }
    }
    OPENAUTO_LOG(error) << "[GSTVideoOutput] Couldn't find a decoder to use!";
    return H264_Decoder::unknown;
}

void GSTVideoOutput::dumpDot()
{    
    gst_debug_bin_to_dot_file(GST_BIN(vidPipeline_), GST_DEBUG_GRAPH_SHOW_VERBOSE, "pipeline");
    OPENAUTO_LOG(info) << "[GSTVideoOutput] Dumped dot debug info";
}

gboolean GSTVideoOutput::busCallback(GstBus*, GstMessage* message, gpointer*)
{
    gchar* debug;
    GError* err;
    gchar* name;

    switch(GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(message, &err, &debug);
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Error " << err->message;
        g_error_free(err);
        g_free(debug);
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(message, &err, &debug);
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Warning " << err->message << " | Debug " << debug;
        name = (gchar*)GST_MESSAGE_SRC_NAME(message);
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Name of src " << (name ? name : "nil");
        g_error_free(err);
        g_free(debug);
        break;
    case GST_MESSAGE_EOS:
        OPENAUTO_LOG(info) << "[GSTVideoOutput] End of stream";
        break;
    case GST_MESSAGE_STATE_CHANGED:
    default:
        break;
    }

    return TRUE;
}

bool GSTVideoOutput::open()
{
    
    
    GstElement* capsFilter = gst_bin_get_by_name(GST_BIN(vidPipeline_), "mycapsfilter");
    GstPad* convertPad = gst_element_get_static_pad(capsFilter, "kmssink");
    gst_pad_add_probe(convertPad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, &GSTVideoOutput::convertProbe, this, nullptr);
    gst_element_set_state(vidPipeline_, GST_STATE_PLAYING);

    checkVideoWidgetVisibility();

    return true;
}

GstPadProbeReturn GSTVideoOutput::convertProbe(GstPad* pad, GstPadProbeInfo* info, void*)
{
    GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
    if(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
    {
        if(GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT)
        {
            GstCaps* caps  = gst_pad_get_current_caps(pad);
            if(caps != nullptr)
            {
                GstVideoInfo* vinfo = gst_video_info_new();
                gst_video_info_from_caps(vinfo, caps);
                OPENAUTO_LOG(info) << "[GSTVideoOutput] Video Width: " << vinfo->width;
                OPENAUTO_LOG(info) << "[GSTVideoOutput] Video Height: " << vinfo->height;
            }

            return GST_PAD_PROBE_REMOVE;
        }
    }

    return GST_PAD_PROBE_OK;
}

bool GSTVideoOutput::init()
{
    OPENAUTO_LOG(info) << "[GSTVideoOutput] init";
    emit startPlayback();
    

    return true;
}

void GSTVideoOutput::write(uint64_t timestamp, const aasdk::common::DataConstBuffer& buffer)
{/*
    if(!firstHeaderParsed && this->configuration_->getWhitescreenWorkaround())
    {
        // I really really really hate this.

        // Raspberry Pi hardware h264 decode appears broken if video_signal_type VUI parameters are given in the h264 header
        // And we don't have control over Android Auto putting these parameters in (which it does.. on some model phones)
        // So we pull in h264bitstream to edit this header on the fly
        
        // This is not a fix, I want to be very clear about that. I don't know what else I'm breaking, or run the 
        // risk of breaking by doing this. This code should only remain here as long as the Pi Engineers haven't released
        // a firmware/driver fix for this yet. An issue has been opened upstream at https://github.com/raspberrypi/firmware/issues/1673

        // Android Auto seems nice enough to always start a message with a new h264 packet,
        // but that doesn't mean we don't have multiple within the message.
        // So if we have a message that _could_ fit two packets (which are delimited by 0x00000001)
        // then we try to find the second and save the data it contains, while editing and replacing the first.
        
        // This header should also always be within the first video message we receive from a device... I think
        
        std::vector<uint8_t> delimit_sequence{0x00, 0x00, 0x00, 0x01};
        std::vector<uint8_t>::iterator sequence_split;
        std::vector<uint8_t> incoming_buffer(&buffer.cdata[0], &buffer.cdata[buffer.size]);

        int nal_start, nal_end;
        uint8_t* buf = (uint8_t *) buffer.cdata;
        int len = buffer.size;
        h264_stream_t* h = h264_new();
        // finds the first NAL packet
        find_nal_unit(buf, len, &nal_start, &nal_end);
        // parses it
        read_nal_unit(h, &buf[nal_start], nal_end - nal_start);
        // wipe all the color description stuff that breaks Pis
        h->sps->vui.video_signal_type_present_flag = 0x00;
        h->sps->vui.video_format = 0x00;
        h->sps->vui.video_full_range_flag = 0x00;
        h->sps->vui.colour_description_present_flag = 0x00;
        h->sps->vui.video_format = 0x00;
        h->sps->vui.video_full_range_flag = 0x00;
        h->sps->vui.colour_description_present_flag = 0x00;
        h->sps->vui.colour_primaries = 0x00;
        h->sps->vui.transfer_characteristics = 0x00;
        h->sps->vui.matrix_coefficients = 0x00;

        // grab some storage
        uint8_t* out_buf = new uint8_t[30];

        // write it to the storage's 3rd index, because h264bitstream seems to have a bug
        // where it both doesn't write the delimiter, and it prepends a leading 0x00
        len = write_nal_unit(h, &out_buf[3], 30) + 3;
        // write the delimiter back
        out_buf[0] = 0x00;
        out_buf[1] = 0x00;
        out_buf[2] = 0x00;
        out_buf[3] = 0x01;

        // output to gstreamer
        GstBuffer* buffer_ = gst_buffer_new_and_alloc(len);
        gst_buffer_fill(buffer_, 0, out_buf, len);
        int ret = gst_app_src_push_buffer((GstAppSrc*)vidSrc_, buffer_);
        if(ret != GST_FLOW_OK)
        {
            OPENAUTO_LOG(info) << "[GSTVideoOutput] Injecting header failed";
        }

        // then check if there's data we need to save
        if(incoming_buffer.size() >= 8){
            sequence_split = std::search(incoming_buffer.begin()+4, incoming_buffer.end(), delimit_sequence.begin(), delimit_sequence.end());
            if(sequence_split != incoming_buffer.end()){
                std::vector<uint8_t> incoming_data_saved(sequence_split, incoming_buffer.end());
                GstBuffer* buffer_ = gst_buffer_new_and_alloc(incoming_data_saved.size());
                gst_buffer_fill(buffer_, 0, incoming_data_saved.data(), incoming_data_saved.size());
                int ret = gst_app_src_push_buffer((GstAppSrc*)vidSrc_, buffer_);
                if(ret != GST_FLOW_OK)
                {
                    OPENAUTO_LOG(info) << "[GSTVideoOutput] Injecting partial header failed";
                }
            }
        }
        OPENAUTO_LOG(info) << "[GSTVideoOutput] Intercepted and replaced h264 header";

        firstHeaderParsed=true;
    }
    else */
    {
        GstBuffer* buffer_ = gst_buffer_new_and_alloc(buffer.size);
        gst_buffer_fill(buffer_, 0, buffer.cdata, buffer.size);
        int ret = gst_app_src_push_buffer((GstAppSrc*)vidSrc_, buffer_);
        if(ret != GST_FLOW_OK)
        {
            OPENAUTO_LOG(info) << "[GSTVideoOutput] push buffer returned " << ret << " for " << buffer.size << "bytes";
        }
    }
}

void GSTVideoOutput::onStartPlayback()
{
    firstHeaderParsed = false;
    if(activeCallback_ != nullptr)
    {
        activeCallback_(true);
    }


    QTimer::singleShot(10000, this, SLOT(dumpDot()));
}

void GSTVideoOutput::stop()
{
    emit stopPlayback();
}

void GSTVideoOutput::onStopPlayback()
{
    firstHeaderParsed = false;

    if(activeCallback_ != nullptr)
    {
        activeCallback_(false);
    }

    OPENAUTO_LOG(info) << "[GSTVideoOutput] stop.";
    // First pause the pipeline
    gst_element_set_state(vidPipeline_, GST_STATE_PAUSED);
    // Then flush any pending buffers in the appsrc
    gst_app_src_end_of_stream(vidSrc_);
    // Finally set to NULL state
    gst_element_set_state(vidPipeline_, GST_STATE_NULL);
}

void GSTVideoOutput::resize()
{
    OPENAUTO_LOG(info) << "[GSTVideoOutput] Got resize request to "<< videoContainer_->width() << "x" << videoContainer_->height();

    if(videoWidget_ != nullptr && videoContainer_ != nullptr)
    {
        videoWidget_->resize(videoContainer_->size());
    }

    int width = 0;
    int height = 0;
    int containerWidth = videoContainer_->width();
    int containerHeight = videoContainer_->height();

    switch(this->getVideoResolution()){
        case aasdk::proto::enums::VideoResolution_Enum__1080p:
            width = 1920;
            height = 1080;
            break;
        case aasdk::proto::enums::VideoResolution_Enum__720p:
            width = 1280;
            height = 720;
            break;
        case aasdk::proto::enums::VideoResolution_Enum__480p:
            width = 800;
            height = 480;
            break;
    }

    double marginWidth = 0;
    double marginHeight = 0;

    double widthRatio = (double)containerWidth / width;
    double heightRatio = (double)containerHeight / height;

    if(widthRatio > heightRatio){
        //cropping height
        marginHeight = (widthRatio * height - containerHeight)/widthRatio;
        marginHeight /= 2;
    }else{
        //cropping width
        marginWidth = (heightRatio * width - containerWidth)/heightRatio;
        marginWidth /= 2;
    }
    

    OPENAUTO_LOG(info) << "[GSTVideoOutput] Android Auto is "<< width << "x" << height << ", calculated margins of: " << marginWidth << "x" << marginHeight;
    g_object_set(vidCrop_, "top", (int)marginHeight, nullptr);
    g_object_set(vidCrop_, "bottom", (int)marginHeight, nullptr);
    g_object_set(vidCrop_, "left", (int)marginWidth, nullptr);
    g_object_set(vidCrop_, "right", (int)marginWidth, nullptr);
    this->configuration_->setVideoMargins(QRect(0,0,(int)(marginWidth*2), (int)(marginHeight*2)));
}



/*
void GSTVideoOutput::checkVideoWidgetVisibility()
{
    if(videoWidget_ != nullptr)
    {
        bool isVisible = videoWidget_->isVisible();
        OPENAUTO_LOG(info) << "[GSTVideoOutput] VideoWidget visibility status: " << (isVisible ? "visible" : "not visible");
        
        // Get kmssink element
        GstElement* kmssink = gst_bin_get_by_name(GST_BIN(vidPipeline_), "kmssink");
        if(kmssink != nullptr)
        {
            if(isVisible)
            {
                // Set full size when visible
                QSize screenSize = QGuiApplication::primaryScreen()->size();
                gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink), 
                screenSize.width()-videoContainer_->width(), 
                screenSize.height()-videoContainer_->height(), 
                videoContainer_->width(), 
                videoContainer_->height());
                OPENAUTO_LOG(info) << "[GSTVideoOutput] Show kmssink";
            }
            else
            {
                // Set reduced size when not visible
                //g_usleep(400000);
                QSize screenSize = QGuiApplication::primaryScreen()->size();
                gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(kmssink), 
                screenSize.width()*2, 
                0, 
                videoContainer_->width(), 
                videoContainer_->height());
                OPENAUTO_LOG(info) << "[GSTVideoOutput] Hide kmssink";
            }
            // Unref the kmssink element
            //gst_object_unref(kmssink);
        }
        else
        {
            OPENAUTO_LOG(info) << "[GSTVideoOutput] Failed to get kmssink element";
        }
    }
    else
    {
        OPENAUTO_LOG(info) << "[GSTVideoOutput] VideoWidget is null";
    }
}
*/

}
}

#endif