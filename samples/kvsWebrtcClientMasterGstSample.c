#include "Samples.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 7000
#define BUFFER_SIZE 8192

int extract_uri_from_json(const char *body, char *out_uri, size_t max_len) {
    const char *key = "\"uri\"";
    const char *start = strstr(body, key);
    if (start) {
        start = strchr(start, ':');
        if (!start) return 0;
        start++; // skip ':'
        while (*start == ' ' || *start == '\"') start++; // skip space/quote

        const char *end = strchr(start, '\"');
        if (!end) return 0;

        size_t len = end - start;
        if (len >= max_len) return 0;

        snprintf(out_uri, max_len, "%.*s", (int)len, start);
        return 1;
    }
    return 0;
}



extern PSampleConfiguration gSampleConfiguration;
// #define VERBOSE

GstElement* senderPipeline = NULL;

// int switch_locker = 0;
// const gchar* curr_uri = "file:///home/adham/Desktop/kinesis/python-samples-for-amazon-kinesis-video-streams-with-webrtc/_assets/input.mp4";
const gchar* curr_uri = "rtsp://admin:gl123456@192.168.0.76:554/Streaming/Channels/1602";
int changing_state = 0;

GstFlowReturn on_new_sample(GstElement* sink, gpointer data, UINT64 trackid)
{
    // printf("on_new_sample called for trackid: %llu\n", trackid);
    GstBuffer* buffer;
    STATUS retStatus = STATUS_SUCCESS;
    BOOL isDroppable, delta;
    GstFlowReturn ret = GST_FLOW_OK;
    GstSample* sample = NULL;
    GstMapInfo info;
    GstSegment* segment;
    GstClockTime buf_pts;
    Frame frame;
    STATUS status;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) data;
    PSampleStreamingSession pSampleStreamingSession = NULL;
    PRtcRtpTransceiver pRtcRtpTransceiver = NULL;
    UINT32 i;
    guint bitrate;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "NULL sample configuration");

    info.data = NULL;
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));

    buffer = gst_sample_get_buffer(sample);
    isDroppable = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED) || GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DECODE_ONLY) ||
        (GST_BUFFER_FLAGS(buffer) == GST_BUFFER_FLAG_DISCONT) ||
        (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT) && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ||
        // drop if buffer contains header only and has invalid timestamp
        !GST_BUFFER_PTS_IS_VALID(buffer);

    if (!isDroppable) {
        delta = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        frame.flags = delta ? FRAME_FLAG_NONE : FRAME_FLAG_KEY_FRAME;

        // convert from segment timestamp to running time in live mode.
        segment = gst_sample_get_segment(sample);
        buf_pts = gst_segment_to_running_time(segment, GST_FORMAT_TIME, buffer->pts);
        if (!GST_CLOCK_TIME_IS_VALID(buf_pts)) {
            DLOGE("[KVS GStreamer Master] Frame contains invalid PTS dropping the frame");
        }

        if (!(gst_buffer_map(buffer, &info, GST_MAP_READ))) {
            DLOGE("[KVS GStreamer Master] on_new_sample(): Gst buffer mapping failed");
            goto CleanUp;
        }

        frame.trackId = trackid;
        frame.duration = 0;
        frame.version = FRAME_CURRENT_VERSION;
        frame.size = (UINT32) info.size;
        frame.frameData = (PBYTE) info.data;

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            pSampleStreamingSession = pSampleConfiguration->sampleStreamingSessionList[i];
            frame.index = (UINT32) ATOMIC_INCREMENT(&pSampleStreamingSession->frameIndex);

            if (trackid == DEFAULT_AUDIO_TRACK_ID) {
                if (pSampleStreamingSession->pSampleConfiguration->enableTwcc && senderPipeline != NULL) {
                    GstElement* encoder = gst_bin_get_by_name(GST_BIN(senderPipeline), "sampleAudioEncoder");
                    if (encoder != NULL) {
                        g_object_get(G_OBJECT(encoder), "bitrate", &bitrate, NULL);
                        MUTEX_LOCK(pSampleStreamingSession->twccMetadata.updateLock);
                        pSampleStreamingSession->twccMetadata.currentAudioBitrate = (UINT64) bitrate;
                        if (pSampleStreamingSession->twccMetadata.newAudioBitrate != 0) {
                            bitrate = (guint) (pSampleStreamingSession->twccMetadata.newAudioBitrate);
                            pSampleStreamingSession->twccMetadata.newAudioBitrate = 0;
                            g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
                        }
                        MUTEX_UNLOCK(pSampleStreamingSession->twccMetadata.updateLock);
                    }
                }
                pRtcRtpTransceiver = pSampleStreamingSession->pAudioRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->audioTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->audioTimestamp +=
                    SAMPLE_AUDIO_FRAME_DURATION; // assume audio frame size is 20ms, which is default in opusenc
            } else {
                if (pSampleStreamingSession->pSampleConfiguration->enableTwcc && senderPipeline != NULL) {
                    GstElement* encoder = gst_bin_get_by_name(GST_BIN(senderPipeline), "sampleVideoEncoder");
                    if (encoder != NULL) {
                        g_object_get(G_OBJECT(encoder), "bitrate", &bitrate, NULL);
                        MUTEX_LOCK(pSampleStreamingSession->twccMetadata.updateLock);
                        pSampleStreamingSession->twccMetadata.currentVideoBitrate = (UINT64) bitrate;
                        if (pSampleStreamingSession->twccMetadata.newVideoBitrate != 0) {
                            bitrate = (guint) (pSampleStreamingSession->twccMetadata.newVideoBitrate);
                            pSampleStreamingSession->twccMetadata.newVideoBitrate = 0;
                            g_object_set(G_OBJECT(encoder), "bitrate", bitrate, NULL);
                        }
                        MUTEX_UNLOCK(pSampleStreamingSession->twccMetadata.updateLock);
                    }
                }
                pRtcRtpTransceiver = pSampleStreamingSession->pVideoRtcRtpTransceiver;
                frame.presentationTs = pSampleStreamingSession->videoTimestamp;
                frame.decodingTs = frame.presentationTs;
                pSampleStreamingSession->videoTimestamp += SAMPLE_VIDEO_FRAME_DURATION; // assume video fps is 25
            }
            status = writeFrame(pRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET && status != STATUS_SUCCESS) {
#ifdef VERBOSE
                DLOGE("[KVS GStreamer Master] writeFrame() failed with 0x%08x", status);
#endif
            } else if (status == STATUS_SUCCESS && pSampleStreamingSession->firstFrame) {
                PROFILE_WITH_START_TIME(pSampleStreamingSession->offerReceiveTime, "Time to first frame");
                pSampleStreamingSession->firstFrame = FALSE;
            } else if (status == STATUS_SRTP_NOT_READY_YET) {
                DLOGI("[KVS GStreamer Master] SRTP not ready yet, dropping frame");
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
    }

CleanUp:

    if (info.data != NULL) {
        gst_buffer_unmap(buffer, &info);
    }

    if (sample != NULL) {
        gst_sample_unref(sample);
    }

    if (ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        ret = GST_FLOW_EOS;
    }

    return ret;
}

GstFlowReturn on_new_sample_video(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_VIDEO_TRACK_ID);
}

GstFlowReturn on_new_sample_audio(GstElement* sink, gpointer data)
{
    return on_new_sample(sink, data, DEFAULT_AUDIO_TRACK_ID);
}

PVOID sendGstreamerAudioVideo(PVOID args)
{
    START_STREAMING:
    STATUS retStatus = STATUS_SUCCESS;
    GError* error = NULL;
    // while(1){
        GstElement *appsinkVideo = NULL, *appsinkAudio = NULL;
        GstBus* bus;
        GstMessage* msg;
        PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;

        CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Gstreamer Master] Streaming session is NULL");

        /**
         * Use x264enc as its available on mac, pi, ubuntu and windows
         * mac pipeline fails if resolution is not 720p
         *
         * For alaw
         * audiotestsrc is-live=TRUE ! queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample !
         * audio/x-raw, rate=8000, channels=1, format=S16LE, layout=interleaved ! alawenc ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio
         *
         * For VP8
         * videotestsrc is-live=TRUE ! video/x-raw,width=1280,height=720,framerate=30/1 !
         * vp8enc error-resilient=partitions keyframe-max-dist=10 auto-alt-ref=true cpu-used=5 deadline=1 !
         * appsink sync=TRUE emit-signals=TRUE name=appsink-video
         *
         *
         * Raspberry Pi Hardware Encode Example
         * "v4l2src device=\"/dev/video0\" ! queue ! v4l2convert ! "
         * "video/x-raw,format=I420,width=640,height=480,framerate=30/1 ! "
         * "v4l2h264enc ! "
         * "h264parse ! "
         * "video/x-h264,stream-format=byte-stream,alignment=au,width=640,height=480,framerate=30/1,profile=baseline,level=(string)4 ! "
         * "appsink sync=TRUE emit-signals=TRUE name=appsink-video"
         */

        CHAR rtspPipeLineBuffer[RTSP_PIPELINE_MAX_CHAR_COUNT];
        printf("Hamza");
        fflush(stdout);
        switch (pSampleConfiguration->mediaType) {
            case SAMPLE_STREAMING_VIDEO_ONLY:
                switch (pSampleConfiguration->srcType) {
                    case TEST_SOURCE: {
                        if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
                            printf("[KVS GStreamer Master] Using H265 codec for video streaming\n");
                            senderPipeline = gst_parse_launch("videotestsrc pattern=ball is-live=TRUE ! timeoverlay ! queue ! videoconvert ! "
                                                            "video/x-raw,width=1280,height=720,framerate=25/1 ! queue ! "
                                                            "x265enc speed-preset=veryfast bitrate=512 tune=zerolatency ! "
                                                            "video/x-h265,stream-format=byte-stream,alignment=au,profile=main ! appsink sync=TRUE "
                                                            "emit-signals=TRUE name=appsink-video",
                                                            &error);
                        } else {
                            printf("[KVS GStreamer Master] Using H264 codec for video streaming\n");
                            gchar pipelineDesc[1024];
                            SNPRINTF(
                                pipelineDesc, sizeof(pipelineDesc),
                                "uridecodebin name=decode uri=\"%s\" ! "
                                "videoconvert ! videoscale ! video/x-raw,width=1280,height=720 ! "
                                "clockoverlay halignment=right valignment=top time-format=\"%%Y-%%m-%%d %%H:%%M:%%S\" ! "
                                "videorate ! video/x-raw,framerate=25/1 ! "
                                "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                                "appsink sync=TRUE emit-signals=TRUE name=appsink-video",
                                curr_uri
                            );
                            senderPipeline = gst_parse_launch(pipelineDesc, &error);

                        }
                        break;
                    }
                    case DEVICE_SOURCE: {
                        printf("[KVS GStreamer Master] Using device source for video streaming\n");
                        senderPipeline = gst_parse_launch(
                            "autovideosrc ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=25/1 ! "
                            "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                            "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                            " appsink sync=TRUE "
                            "emit-signals=TRUE name=appsink-video",
                            &error);
                        break;
                    }
                    case RTSP_SOURCE: {
                        printf("[KVS GStreamer Master] Using RTSP source for video streaming\n");
                        UINT16 stringOutcome =
                            SNPRINTF(rtspPipeLineBuffer, RTSP_PIPELINE_MAX_CHAR_COUNT,
                                    "uridecodebin name=decode  uri=%s ! "
                                    "videoconvert ! "
                                    "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! queue ! "
                                    "appsink sync=TRUE emit-signals=TRUE name=appsink-video ",
                                    curr_uri);

                        if (stringOutcome > RTSP_PIPELINE_MAX_CHAR_COUNT) {
                            DLOGE("[KVS GStreamer Master] ERROR: rtsp uri entered exceeds maximum allowed length set by RTSP_PIPELINE_MAX_CHAR_COUNT");
                            goto CleanUp;
                        }
                        senderPipeline = gst_parse_launch(rtspPipeLineBuffer, &error);

                        break;
                    }
                }
                break;

            case SAMPLE_STREAMING_AUDIO_VIDEO:
                switch (pSampleConfiguration->srcType) {
                    case TEST_SOURCE: {
                        printf("Adham Ali");
                        // flush stdout
                        fflush(stdout);
                        if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE &&
                            pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
                            senderPipeline = gst_parse_launch(
                                "videotestsrc pattern=ball is-live=TRUE ! "
                                "queue ! videorate ! videoscale ! videoconvert ! video/x-raw,width=1280,height=720,framerate=25/1 ! "
                                "clockoverlay halignment=right valignment=top time-format=\"%Y-%m-%d %H:%M:%S\" ! "
                                "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! "
                                "appsink sync=TRUE emit-signals=TRUE name=appsink-video audiotestsrc wave=ticks is-live=TRUE ! "
                                "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc name=sampleAudioEncoder ! "
                                "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                                &error);
                        } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265 && pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
                            senderPipeline =
                                gst_parse_launch("videotestsrc pattern=ball is-live=TRUE ! timeoverlay ! queue ! videoconvert ! "
                                                "video/x-raw,width=1280,height=720,framerate=25/1 ! queue ! "
                                                "x265enc speed-preset=veryfast bitrate=512 tune=zerolatency ! "
                                                "video/x-h265,stream-format=byte-stream,alignment=au,profile=main ! appsink sync=TRUE "
                                                "emit-signals=TRUE name=appsink-video audiotestsrc is-live=TRUE ! "
                                                "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc ! "
                                                "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                                                &error);
                        }
                        // TODO: test and add more such combinations
                        break;
                    }
                    case DEVICE_SOURCE: {
                        senderPipeline = gst_parse_launch(
                            "autovideosrc ! queue ! videoconvert ! video/x-raw,width=1280,height=720,framerate=25/1 ! "
                            "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                            "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! appsink sync=TRUE emit-signals=TRUE "
                            "name=appsink-video autoaudiosrc ! "
                            "queue leaky=2 max-size-buffers=400 ! audioconvert ! audioresample ! opusenc name=sampleAudioEncoder ! "
                            "audio/x-opus,rate=48000,channels=2 ! appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                            &error);
                        break;
                    }
                    case RTSP_SOURCE: {
                        UINT16 stringOutcome =
                            SNPRINTF(rtspPipeLineBuffer, RTSP_PIPELINE_MAX_CHAR_COUNT,
                                    "uridecodebin uri=%s name=decode ! videoconvert ! "
                                    "x264enc name=sampleVideoEncoder bframes=0 speed-preset=veryfast bitrate=512 byte-stream=TRUE tune=zerolatency ! "
                                    "video/x-h264,stream-format=byte-stream,alignment=au,profile=baseline ! queue ! "
                                    "appsink sync=TRUE emit-signals=TRUE name=appsink-video "
                                    "src. ! audioconvert ! "
                                    "audioresample ! opusenc name=sampleAudioEncoder ! audio/x-opus,rate=48000,channels=2 ! queue ! "
                                    "appsink sync=TRUE emit-signals=TRUE name=appsink-audio",
                                    curr_uri);

                        if (stringOutcome > RTSP_PIPELINE_MAX_CHAR_COUNT) {
                            DLOGE("[KVS GStreamer Master] ERROR: rtsp uri entered exceeds maximum allowed length set by RTSP_PIPELINE_MAX_CHAR_COUNT");
                            goto CleanUp;
                        }
                        senderPipeline = gst_parse_launch(rtspPipeLineBuffer, &error);

                        break;
                    }
                }
                break;
        }

        CHK_ERR(senderPipeline != NULL, STATUS_NULL_ARG, "[KVS Gstreamer Master] Pipeline is NULL");

        appsinkVideo = gst_bin_get_by_name(GST_BIN(senderPipeline), "appsink-video");
        appsinkAudio = gst_bin_get_by_name(GST_BIN(senderPipeline), "appsink-audio");

        if (!(appsinkVideo != NULL || appsinkAudio != NULL)) {
            DLOGE("[KVS GStreamer Master] sendGstreamerAudioVideo(): cant find appsink, operation returned status code: 0x%08x", STATUS_INTERNAL_ERROR);
            goto CleanUp;
        }

        if (appsinkVideo != NULL) {
            g_signal_connect(appsinkVideo, "new-sample", G_CALLBACK(on_new_sample_video), (gpointer) pSampleConfiguration);
        }
        if (appsinkAudio != NULL) {
            g_signal_connect(appsinkAudio, "new-sample", G_CALLBACK(on_new_sample_audio), (gpointer) pSampleConfiguration);
        }
        gst_element_set_state(senderPipeline, GST_STATE_PLAYING);

        /* block until error or EOS */
        bus = gst_element_get_bus(senderPipeline);
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
        printf("[KVS GStreamer Master] sendGstreamerAudioVideo(): Pipeline started, waiting for EOS or error Aliiiii\n");
        /* Free resources */
        if (msg != NULL) {
            gst_message_unref(msg);
        }
        if (bus != NULL) {
            gst_object_unref(bus);
        }
        if (senderPipeline != NULL) {
            gst_element_set_state(senderPipeline, GST_STATE_NULL);
            gst_object_unref(senderPipeline);
        }
        if (appsinkAudio != NULL) {
            gst_object_unref(appsinkAudio);
        }
        if (appsinkVideo != NULL) {
            gst_object_unref(appsinkVideo);
        }
    // }
CleanUp:

    if (error != NULL) {
        DLOGE("[KVS GStreamer Master] %s", error->message);
        g_clear_error(&error);
    }
    printf("changing state is %d\n", changing_state);
    if (changing_state){
        changing_state = 0;
        goto START_STREAMING;
    }

    return (PVOID) (ULONG_PTR) retStatus;
}
void change_uri(const gchar* new_uri) {
    if (senderPipeline == NULL || !GST_IS_BIN(senderPipeline)) {
        g_printerr("senderPipeline is NULL or not a GstBin. Cannot switch URI.\n");
        return;
    }

    GstElement* uridecodebin = gst_bin_get_by_name(GST_BIN(senderPipeline), "decode");

    if (uridecodebin != NULL) {
        changing_state = 1;
        curr_uri= new_uri;
        gst_element_set_state(uridecodebin, GST_STATE_NULL);
        // g_object_set(G_OBJECT(uridecodebin), "uri", new_uri, NULL);
        gst_element_set_state(uridecodebin, GST_STATE_PLAYING);
        // sleep(1);
        // gst_object_unref(uridecodebin);
        // g_print("Switched URI to: %s\n", new_uri);
    } else {
        g_printerr("Failed to find 'decode' element to switch URI.\n");
    }
}


void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];
    int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        perror("read");
        close(client_socket);
        return;
    }

    buffer[bytes_read] = '\0';

    // Accept any PATCH method
    if (strncmp(buffer, "PATCH", 5) == 0) {
        char *body = strstr(buffer, "\r\n\r\n");
        if (body) {
            body += 4;
            char uri[1024];
            int ok = extract_uri_from_json(body, uri, sizeof(uri));
            if (ok) {
                printf("Extracted URI: %s\n", uri);
                change_uri(uri);

                char json_response[2048];
                snprintf(json_response, sizeof(json_response),
                         "{ \"status\": \"ok\", \"uri\": \"%s\" }\n", uri);

                char header[256];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n"
                         "\r\n", strlen(json_response));

                write(client_socket, header, strlen(header));
                write(client_socket, json_response, strlen(json_response));
            } else {
                const char *json_response = "{ \"status\": \"error\", \"message\": \"URI parse failed\" }\n";

                char header[256];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 500 Internal Server Error\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n"
                         "\r\n", strlen(json_response));

                write(client_socket, header, strlen(header));
                write(client_socket, json_response, strlen(json_response));
            }
        } else {
            const char *json_response = "{ \"status\": \"error\", \"message\": \"Missing body\" }\n";

            char header[256];
            snprintf(header, sizeof(header),
                     "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n", strlen(json_response));

            write(client_socket, header, strlen(header));
            write(client_socket, json_response, strlen(json_response));
        }
    } else {
        const char *json_response = "{ \"status\": \"error\", \"message\": \"Method not allowed\" }\n";

        char header[256];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 405 Method Not Allowed\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n", strlen(json_response));

        write(client_socket, header, strlen(header));
        write(client_socket, json_response, strlen(json_response));
    }

    close(client_socket);
}


int server() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 3) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d...\n", PORT);

    while (1) {
        client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }

        handle_client(client_socket);
    }

    return 0;
}

void* filesrc_switcher_thread(void* arg) {
    // const gchar* files[] = {
    //     "rtsp://admin:gl123456@192.168.0.76:554/Streaming/Channels/1602",
    //     "rtsp://admin:gl123456@192.168.0.76:554/Streaming/Channels/102",
    // };
    // int index = 0;
    // int fileCount = 2;

    while (!ATOMIC_LOAD_BOOL(&gSampleConfiguration->appTerminateFlag)) {
        server();
    }

    return NULL;
}

INT32 main(INT32 argc, CHAR* argv[])
{
    printf("Adham\n");
    // flush the stdout buffer to ensure that the output is printed immediately
    fflush(stdout);
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();

    signal(SIGINT, sigintHandler);

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, TRUE, logLevel, &pSampleConfiguration));

    if (argc > 3 && STRCMP(argv[3], "testsrc") == 0) {
        if (argc > 4) {
            if (!STRCMP(argv[4], AUDIO_CODEC_NAME_OPUS)) {
                audioCodec = RTC_CODEC_OPUS;
            }
        }

        if (argc > 5) {
            if (!STRCMP(argv[5], VIDEO_CODEC_NAME_H265)) {
                videoCodec = RTC_CODEC_H265;
            }
        }
    }

    pSampleConfiguration->videoSource = sendGstreamerAudioVideo;
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif
    pSampleConfiguration->customData = (UINT64) pSampleConfiguration;
    pSampleConfiguration->srcType = DEVICE_SOURCE; // Default to device source (autovideosrc and autoaudiosrc)
    /* Initialize GStreamer */
    gst_init(&argc, &argv);
    DLOGI("[KVS Gstreamer Master] Finished initializing GStreamer and handlers");

    if (argc > 2) {
        if (STRCMP(argv[2], "video-only") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_VIDEO_ONLY;
            DLOGI("[KVS Gstreamer Master] Streaming video only");
        } else if (STRCMP(argv[2], "audio-video-storage") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            pSampleConfiguration->channelInfo.useMediaStorage = TRUE;
            DLOGI("[KVS Gstreamer Master] Streaming audio and video");
        } else if (STRCMP(argv[2], "audio-video") == 0) {
            pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
            DLOGI("[KVS Gstreamer Master] Streaming audio and video");
        } else {
            DLOGI("[KVS Gstreamer Master] Unrecognized streaming type. Default to video-only");
        }
    } else {
        DLOGI("[KVS Gstreamer Master] Streaming video only");
    }

    if (argc > 3) {
        if (STRCMP(argv[3], "testsrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using test source in GStreamer");
            pSampleConfiguration->srcType = TEST_SOURCE;
        } else if (STRCMP(argv[3], "devicesrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using device source in GStreamer");
            pSampleConfiguration->srcType = DEVICE_SOURCE;
        } else if (STRCMP(argv[3], "rtspsrc") == 0) {
            DLOGI("[KVS GStreamer Master] Using RTSP source in GStreamer");
            if (argc < 5) {
                DLOGI("[KVS GStreamer Master] No RTSP source URI included. Defaulting to device source");
                DLOGI("[KVS GStreamer Master] Usage: ./kvsWebrtcClientMasterGstSample <channel name> audio-video rtspsrc rtsp://<rtsp uri>"
                      "or ./kvsWebrtcClientMasterGstSample <channel name> video-only rtspsrc <rtsp://<rtsp uri>");
                pSampleConfiguration->srcType = DEVICE_SOURCE;
            } else {
                pSampleConfiguration->srcType = RTSP_SOURCE;
                pSampleConfiguration->rtspUri = argv[4];
            }
        } else {
            DLOGI("[KVS Gstreamer Master] Unrecognized source type. Defaulting to device source in GStreamer");
        }
    } else {
        DLOGI("[KVS GStreamer Master] Using device source in GStreamer");
    }

    switch (pSampleConfiguration->mediaType) {
        case SAMPLE_STREAMING_VIDEO_ONLY:
            DLOGI("[KVS GStreamer Master] streaming type video-only");
            break;
        case SAMPLE_STREAMING_AUDIO_VIDEO:
            DLOGI("[KVS GStreamer Master] streaming type audio-video");
            break;
    }

    // Initalize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS GStreamer Master] KVS WebRTC initialization completed successfully");

    CHK_STATUS(initSignaling(pSampleConfiguration, SAMPLE_MASTER_CLIENT_ID));
    DLOGI("[KVS GStreamer Master] Channel %s set up done ", pChannelName);
    pthread_t fileSwitcherTid;
    pthread_create(&fileSwitcherTid, NULL, filesrc_switcher_thread, NULL);

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
    DLOGI("[KVS GStreamer Master] Streaming session terminated");

    // pthread_join(fileSwitcherTid, NULL);
    // DLOGI("[KVS GStreamer Master] File switcher thread terminated");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS GStreamer Master] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS GStreamer Master] Cleaning up....");

    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
        }

        if (pSampleConfiguration->enableFileLogging) {
            freeFileLogger();
        }
        retStatus = freeSignalingClient(&pSampleConfiguration->signalingClientHandle);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS GStreamer Master] freeSignalingClient(): operation returned status code: 0x%08x", retStatus);
        }

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS GStreamer Master] freeSampleConfiguration(): operation returned status code: 0x%08x", retStatus);
        }
    }
    DLOGI("[KVS Gstreamer Master] Cleanup done");

    RESET_INSTRUMENTED_ALLOCATORS();

    // https://www.gnu.org/software/libc/manual/html_node/Exit-Status.html
    // We can only return with 0 - 127. Some platforms treat exit code >= 128
    // to be a success code, which might give an unintended behaviour.
    // Some platforms also treat 1 or 0 differently, so it's better to use
    // EXIT_FAILURE and EXIT_SUCCESS macros for portability.
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}
