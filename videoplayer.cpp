#include "videoplayer.h"
#include <stdio.h>
#include <QDebug>
#include <QDateTime>
#define FLUSH_DATA       "FLUSH"
#define SDL_AUDIO_BUFFER_SIZE 1024
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio


/*****队列初始化*****/
void packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
    q->size = 0;
    q->nb_packets = 0;
    q->first_pkt = NULL;
    q->last_pkt = NULL;
}

/*****数据压入队列*****/
int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

    AVPacketList *pkt1;
    if (av_dup_packet(pkt) < 0) {
        return -1;
    }
    pkt1 = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    SDL_LockMutex(q->mutex);
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);//通过条件变量 发送信号
    SDL_UnlockMutex(q->mutex);
    return 0;
}

/*****数据出队列*****/
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
    AVPacketList *pkt1;
    int ret = 0;

    SDL_LockMutex(q->mutex);

    for (;;) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

/*****清空队列*****/
static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1)
    {
        pkt1 = pkt->next;

        if(pkt1->pkt.data != (uint8_t *)"FLUSH")
        {
            /**do nothing ?**/
        }
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);

    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    SDL_UnlockMutex(q->mutex);
}

/*****音频解码函数*****/
int audio_decode_frame(VideoState *m_pstVideosState,double *pts_ptr)
{

    int n;
    int len1;
    int len2;
    int decoded_data_size;
    int got_frame = 0;
    int wanted_nb_samples;
    int resampled_data_size;
    double pts;
    int64_t dec_channel_layout;
    AVPacket *pkt = &m_pstVideosState->audio_pkt;

    for(;;)
    {
        while(m_pstVideosState->audio_pkt_size > 0)
        {
            //需加入暂停
            /**为audio_frame分配空间**/
            if (!m_pstVideosState->audio_frame) {
                if (!(m_pstVideosState->audio_frame = av_frame_alloc()))
                {
                    return AVERROR(ENOMEM);
                }
            }
            else{
                //取消引用帧引用的所有缓冲区并重置帧字段。
                av_frame_unref(m_pstVideosState->audio_frame);
            }
            //将大小为pkt->size from pkt->data的音频帧解码为音频帧。
            len1 = avcodec_decode_audio4( m_pstVideosState->audio_st->codec,
                                          m_pstVideosState->audio_frame,
                                          &got_frame, pkt);
            if( len1 < 0 ) {
                m_pstVideosState->audio_pkt_size = 0;
                break;

            }
            m_pstVideosState->audio_pkt_data += len1; /**更新data**/
            m_pstVideosState->audio_pkt_size -= len1; /**更新size**/
            if (!got_frame) continue;/**获取帧失败**/
            /**计算解码出来的桢需要的缓冲大小**/
            decoded_data_size = av_samples_get_buffer_size(NULL,
                                    m_pstVideosState->audio_frame->channels,
                                    m_pstVideosState->audio_frame->nb_samples,
                                    (AVSampleFormat)m_pstVideosState->audio_frame->format, 1);
            /**通道配置**/
            dec_channel_layout =(m_pstVideosState->audio_frame->channel_layout
                        && m_pstVideosState->audio_frame->channels==
                        av_get_channel_layout_nb_channels(m_pstVideosState->audio_frame->channel_layout)) ?
                        m_pstVideosState->audio_frame->channel_layout :
                        av_get_default_channel_layout(m_pstVideosState->audio_frame->channels);
            //此帧描述的音频采样数（每个通道）
            wanted_nb_samples = m_pstVideosState->audio_frame->nb_samples;
            if (m_pstVideosState->audio_frame->format != m_pstVideosState->audio_src_fmt
                    || dec_channel_layout != m_pstVideosState->audio_src_channel_layout
                    || m_pstVideosState->audio_frame->sample_rate != m_pstVideosState->audio_src_freq
                    || (wanted_nb_samples != m_pstVideosState->audio_frame->nb_samples
                    && !m_pstVideosState->swr_ctx)){
                if (m_pstVideosState->swr_ctx)
                    swr_free(&m_pstVideosState->swr_ctx);
                /**Allocate SwrContext if needed and set/reset common parameters**/
                m_pstVideosState->swr_ctx = swr_alloc_set_opts(NULL,
                        m_pstVideosState->audio_tgt_channel_layout, (AVSampleFormat)m_pstVideosState->audio_tgt_fmt,
                        m_pstVideosState->audio_tgt_freq, dec_channel_layout,
                        (AVSampleFormat)m_pstVideosState->audio_frame->format, m_pstVideosState->audio_frame->sample_rate,
                        0, NULL);
                if (!m_pstVideosState->swr_ctx || swr_init(m_pstVideosState->swr_ctx) < 0){
                    printf("swr_init() failed\n");
                    break;
                }
                m_pstVideosState->audio_src_channel_layout = dec_channel_layout;
                m_pstVideosState->audio_src_channels = m_pstVideosState->audio_st->codec->channels;
                m_pstVideosState->audio_src_freq = m_pstVideosState->audio_st->codec->sample_rate;
                m_pstVideosState->audio_src_fmt = m_pstVideosState->audio_st->codec->sample_fmt;
            }
            /* 这里我们可以对采样数进行调整，增加或者减少，一般可以用来做声画同步 */
            if (m_pstVideosState->swr_ctx){
                const uint8_t **in = (const uint8_t **) m_pstVideosState->audio_frame->extended_data;
                uint8_t *out[] = { m_pstVideosState->audio_buf2 };
                if (wanted_nb_samples != m_pstVideosState->audio_frame->nb_samples)
                {
                    if (swr_set_compensation(m_pstVideosState->swr_ctx,
                            (wanted_nb_samples - m_pstVideosState->audio_frame->nb_samples)
                                    * m_pstVideosState->audio_tgt_freq
                                    / m_pstVideosState->audio_frame->sample_rate,
                            wanted_nb_samples * m_pstVideosState->audio_tgt_freq
                                    / m_pstVideosState->audio_frame->sample_rate) < 0){
                        printf("swr_set_compensation() failed\n");
                        break;
                    }
                }
                len2 = swr_convert(m_pstVideosState->swr_ctx, out,
                        sizeof(m_pstVideosState->audio_buf2) / m_pstVideosState->audio_tgt_channels / av_get_bytes_per_sample(m_pstVideosState->audio_tgt_fmt),
                        in, m_pstVideosState->audio_frame->nb_samples);
                if (len2 < 0) {
                    printf("swr_convert() failed\n");
                    break;
                }
                if (len2== sizeof(m_pstVideosState->audio_buf2) / m_pstVideosState->audio_tgt_channels
                        / av_get_bytes_per_sample(m_pstVideosState->audio_tgt_fmt)){
                    swr_init(m_pstVideosState->swr_ctx);
                }
                m_pstVideosState->audio_buf = m_pstVideosState->audio_buf2;
                resampled_data_size = len2 * m_pstVideosState->audio_tgt_channels
                        * av_get_bytes_per_sample(m_pstVideosState->audio_tgt_fmt);
            }
            else{
                resampled_data_size = decoded_data_size;
                m_pstVideosState->audio_buf = m_pstVideosState->audio_frame->data[0];
            }
            //m_pstVideosState->player->SetVolume(m_pstVideosState->audio_buf,resampled_data_size,1,m_pstVideosState->player->m_vol);
            pts = m_pstVideosState->audio_clock;
            *pts_ptr = pts;
            n = 2 * m_pstVideosState->audio_st->codec->channels;
            m_pstVideosState->audio_clock += (double) resampled_data_size
                    / (double) (n * m_pstVideosState->audio_st->codec->sample_rate);

            return resampled_data_size;
        }
        if (pkt->data)
            av_free_packet(pkt);
        memset(pkt, 0, sizeof(*pkt));

        if (m_pstVideosState->quit)
            return -1;
        if (packet_queue_get(&m_pstVideosState->audioq, pkt, 0) <= 0)
            return -1;
        /**收到这个数据 说明刚刚执行过跳转 现在需要把解码器的数据 清除一下**/
        if(strcmp((char*)pkt->data,FLUSH_DATA) == 0)
        {
            avcodec_flush_buffers(m_pstVideosState->audio_st->codec);
            av_free_packet(pkt);
            continue;
        }
        m_pstVideosState->audio_pkt_data = pkt->data;
        m_pstVideosState->audio_pkt_size = pkt->size;
        /**if update, update the audio clock w/pts **/
        if (pkt->pts != AV_NOPTS_VALUE)
            m_pstVideosState->audio_clock = av_q2d(m_pstVideosState->audio_st->time_base) * pkt->pts;

   }
    return 0;
}

/*****音频解码回调函数*****/
void audio_callback(void *userdata, Uint8 *stream, int len)
{
    VideoState *m_pstVideosState = (VideoState *) userdata;

    int len1, audio_data_size;
    double pts;

    /*   len是由SDL传入的SDL缓冲区的大小，如果这个缓冲未满，我们就一直往里填充数据 */
    while (len > 0) {
        /*  audio_buf_index 和 audio_buf_size 标示我们自己用来放置解码出来的数据的缓冲区，*/
        /*   这些数据待copy到SDL缓冲区， 当audio_buf_index >= audio_buf_size的时候意味着我*/
        /*   们的缓冲为空，没有数据可供copy，这时候需要调用audio_decode_frame来解码出更
            多的桢数据 */
        m_pstVideosState->player->baudio = false;
        if (m_pstVideosState->audio_buf_index >= m_pstVideosState->audio_buf_size) {
            audio_data_size = audio_decode_frame(m_pstVideosState, &pts);
            if (audio_data_size < 0) {/* audio_data_size < 0 标示没能解码出数据，我们默认播放静音 */
                /* silence */
                m_pstVideosState->audio_buf_size = 1024;
                /* 清零，静音 */
                if (m_pstVideosState->audio_buf == NULL) {
                    m_pstVideosState->player->baudio = true;
                    printf("audio decode error \n");
                    return;
                }
                memset(m_pstVideosState->audio_buf, 0, m_pstVideosState->audio_buf_size);
            }
            else {
                m_pstVideosState->audio_buf_size = audio_data_size;
            }
            m_pstVideosState->audio_buf_index = 0;
        }
        /*  查看stream可用空间，决定一次copy多少数据，剩下的下次继续copy */
        len1 = m_pstVideosState->audio_buf_size - m_pstVideosState->audio_buf_index;
        if (len1 > len) len1 = len;
        if (m_pstVideosState->audio_buf == NULL){
            printf("audio buf is full \n");
            return;
        }

        memcpy(stream, (uint8_t *) m_pstVideosState->audio_buf + m_pstVideosState->audio_buf_index, len1);
        len -= len1;
        stream += len1;
        m_pstVideosState->audio_buf_index += len1;
    }
    //如果只有音频播放，则需要增加一个音频解析完成的退出条件
    if(m_pstVideosState->s32VStreamCount < 0){
        if(m_pstVideosState->audio_clock >= (m_pstVideosState->pFormatContext->duration/1000000)){
            m_pstVideosState->player->bPlayend = true;
            m_pstVideosState->quit = true;
        }
    }
}

static double synchronize_video(VideoState *is, AVFrame *src_frame, double pts)
{
    double frame_delay;

    if (pts != 0) {
        /* if we have pts, set video clock to it */
        is->video_clock = pts;
    } else {
        /* if we aren't given a pts, set it to the clock */
        pts = is->video_clock;
    }
    /* update the video clock */
    frame_delay = av_q2d(is->video_st->codec->time_base);
    /* if we are repeating a frame, adjust clock accordingly */
    frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
    is->video_clock += frame_delay;
    return pts;
}

/*******初始化音频解码器，初始化音频存储队列*******/
int audio_stream_component_open(VideoState *m_pstVideosState, uint stream_index)
{
    AVCodecContext *codecCtx;
    AVCodec *codec;
    SDL_AudioSpec spec;         /**输出实际音频信息**/
    SDL_AudioSpec wanted_spec;  /**输入音频信息**/
    AVFormatContext *pFormatContext = m_pstVideosState->pFormatContext;
    int wanted_nb_channels;
    uint64_t wanted_channel_layout = 0;

    /**SDL支持的声道数为 1, 2, 4, 6 **/
    /**后面我们会使用这个数组来纠正不支持的声道数目**/
    const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };

    if (stream_index < 0 || stream_index >= pFormatContext->nb_streams){
        return -1;
    }

    codecCtx = pFormatContext->streams[stream_index]->codec;
    wanted_nb_channels = codecCtx->channels;
    if (!wanted_channel_layout|| wanted_nb_channels
        != av_get_channel_layout_nb_channels(wanted_channel_layout))
    {
       wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
       wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    /**音频通道设置**/
    wanted_spec.channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.freq = codecCtx->sample_rate;
    /**没有音频信息**/
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0){
        fprintf(stderr,"Invalid sample rate or channel count!\n");
        return -1;
    }
    wanted_spec.format = AUDIO_S16SYS;            /**Signed 16-bit samples**/
    wanted_spec.silence = 0;                      /**0指示静音**/
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  /**自定义SDL缓冲区大小 1024**/
    /**This function is called when the audio device needs more data**/
    wanted_spec.callback = audio_callback;        /**音频解码的关键回调函数**/
    wanted_spec.userdata = m_pstVideosState;      /**传给上面回调函数的外带数据**/
    Uint8 utempchannels = 0;
    do
    {
        /**Open a specific audio device，正常时返回值不为0**/
        //打开音频设备，SDL_OpenAudioDevice函数只能用来打开AudioDeviceID大于等于2的设备， 而默认的设备，即AudioDeviceID等于1，只能用SDL_OpenAudio函数打开
        m_pstVideosState->audioID = SDL_OpenAudioDevice(SDL_GetAudioDeviceName(1,0),0,&wanted_spec, &spec,0);
        printf("m_pstVideosState->audioID = %d\n",m_pstVideosState->audioID);
        /**打印通道信息**/
        fprintf(stderr,"SDL_OpenAudio (%d channels): %s\n",wanted_spec.channels, SDL_GetError());
        /**通道合法性判断**/
        utempchannels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!utempchannels){
            printf("No more channel combinations to tyu, audio open failed\n");
            break;
        }
        /**Return default channel layout for a given number of channels**/
        wanted_channel_layout = av_get_default_channel_layout(utempchannels);
    }while(m_pstVideosState->audioID == 0);
    wanted_spec.channels = utempchannels;
    /**检查实际使用的配置（保存在spec,由SDL_OpenAudio()填充）**/
    if (spec.format != AUDIO_S16SYS) {
        fprintf(stderr,"SDL advised audio format %d m_pstVideosState not supported!\n",spec.format);
        return -1;
    }

    /**通道判断**/
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            fprintf(stderr,"SDL advised channel count %d m_pstVideosState not supported!\n",spec.channels);
            return -1;
        }
    }
    m_pstVideosState->audio_hw_buf_size = spec.size;

    /**把设置好的参数保存到大结构中**/
    m_pstVideosState->audio_src_fmt = m_pstVideosState->audio_tgt_fmt = AV_SAMPLE_FMT_S16;
    m_pstVideosState->audio_src_freq = m_pstVideosState->audio_tgt_freq = spec.freq;
    m_pstVideosState->audio_src_channel_layout = m_pstVideosState->audio_tgt_channel_layout = wanted_channel_layout;
    m_pstVideosState->audio_src_channels = m_pstVideosState->audio_tgt_channels = spec.channels;

    /** Find a registered decoder with a matching codec ID.**/
    codec = avcodec_find_decoder(codecCtx->codec_id);
    /**open codec**/
    if (!codec || (avcodec_open2(codecCtx, codec, NULL) < 0)) {
        fprintf(stderr,"Unsupported codec!\n");
        return -1;
    }
    /**设置丢弃没有用的音频包**/
    pFormatContext->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    switch (codecCtx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
        {
            //        m_pstVideosState->s32AStreamCount = stream_index;
            m_pstVideosState->audio_st = pFormatContext->streams[stream_index];
            m_pstVideosState->audio_buf_size = 1024;
            m_pstVideosState->audio_buf_index = 4096;
            memset(&m_pstVideosState->audio_pkt, 0, sizeof(m_pstVideosState->audio_pkt));

            /**队列初始化**/
            packet_queue_init(&m_pstVideosState->audioq);
            SDL_PauseAudioDevice(m_pstVideosState->audioID,0);
            break;
        }

        default:
            break;
    }
    return 0;
}
/*****视频解码线程******/
int video_thread(void *arg){
    int ret;
    int got_picture;
    int numBytes;
    double video_pts = 0; /**当前视频的pts**/
    double audio_pts = 0; /**音频pts**/

    AVPacket pkt1;
    AVPacket *packet = &pkt1;
    VideoState *m_pstVideosState = (VideoState *) arg;

    /**解码视频相关**/
    AVFrame *pFrame; /**解码器解码之后的图像**/
    AVFrame *pFrameRGB;/**转换编码之后的视频流**/
    uint8_t *out_buffer_rgb; /**解码后的图像数据**/
    struct SwsContext *img_convert_ctx;  /**用于解码后的视频格式转换**/

    AVCodecContext *l_pstVCodecCtx = m_pstVideosState->video_st->codec; /**视频解码器上下文**/

    pFrame = av_frame_alloc();//分配空间
    pFrameRGB = av_frame_alloc();//分配空间

    int iwidth = l_pstVCodecCtx->coded_width;
    int iheight = l_pstVCodecCtx->height;

    img_convert_ctx = sws_getContext(iwidth, iheight,
            l_pstVCodecCtx->pix_fmt, iwidth, iheight,
            AV_PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);
    numBytes = avpicture_get_size(AV_PIX_FMT_RGB32, iwidth,iheight);
    //numBytes = avpicture_get_size(PIX_FMT_RGB32, iwidth,iheight);
    out_buffer_rgb = (uint8_t *) av_malloc(numBytes * sizeof(uint8_t));
     //该函数会自动填充AVFrame的data和linesize字段
    avpicture_fill((AVPicture *) pFrameRGB, out_buffer_rgb, AV_PIX_FMT_RGB32,iwidth, iheight);

    while(1){
        //判断退出&判断暂停
        /**判断退出**/
        if (m_pstVideosState->quit){
            qDebug()<<"video_thread quit\t"<< m_pstVideosState->player->PrintCurrentTime();
            break;
        }
        if (packet_queue_get(&m_pstVideosState->videoq, packet, 0) <= 0){
            if (m_pstVideosState->readFinished){
                /**队列里面没有数据了且读取完毕了**/
                m_pstVideosState->player->bPlayend = true;
                SDL_Delay(100);
                qDebug()<<"video_thread readFinished\t"<< m_pstVideosState->player->PrintCurrentTime();
                break;
            }
            else{/**队列只是暂时没有数据而已**/
                /**延时,ms 单位**/
                SDL_Delay(1);
                continue;
            }
        }
        /**收到这个数据 说明刚刚执行过跳转 现在需要把解码器的数据 清除一下**/
        if(strcmp((char*)packet->data,FLUSH_DATA) == 0){
            avcodec_flush_buffers(m_pstVideosState->video_st->codec);
            av_free_packet(packet);
            continue;
        }
        /**视频解码**/
        /**将packet->data中packet->size 字节的数据解码到 pFrame 中**/
        ret = avcodec_decode_video2(l_pstVCodecCtx, pFrame, &got_picture,packet);
        /**解码失败**/
        if (ret < 0) {
            qDebug()<<"decode error.\n";
            av_free_packet(packet);
            continue;
        }
        /**解码时间无效**/
        if (packet->dts == AV_NOPTS_VALUE && pFrame->opaque&& *(uint64_t*) pFrame->opaque != AV_NOPTS_VALUE)
        {
            video_pts = *(uint64_t *) pFrame->opaque;
        }/**解码时间有效**/
        else if (packet->dts != AV_NOPTS_VALUE)
            video_pts = packet->dts;/**解码时间戳赋值给显示时间戳**/
        else
            video_pts = 0;/**显示时间戳赋值为0**/
        /**AVRational 转换为double 数据类型**/
        video_pts *= av_q2d(m_pstVideosState->video_st->time_base);
        /**同步视频时间**/
        video_pts = synchronize_video(m_pstVideosState, pFrame, video_pts);

        /**跳转标志 -- 用于视频线程中，run 线程中设置该标志**/
        //if (m_pstVideosState->seek_flag_video)
        /**循环等待视频和视频的时间同步
        有个问题：如果音频解码失败，音频pts一直为0 ，者会播放卡住**/
        int iAlldelayTime = 0;
        //如果某些视频出现最终的音频时间小于视频时间则会一直卡在这个循环，所以这里增加一个退出条件，最多再延时100毫秒
        while(!m_pstVideosState->player->baudio && iAlldelayTime < 100){
            /**判断是否退出**/
            if (m_pstVideosState->quit)
                break;
            audio_pts = m_pstVideosState->audio_clock;

            /**主要是 跳转的时候 我们把video_clock设置成0了
            因此这里需要更新video_pts
            否则当从后面跳转到前面的时候 会卡在这里**/

            video_pts = m_pstVideosState->video_clock;

            if(video_pts <= audio_pts)
                break;

            int delayTime = (video_pts - audio_pts) * 1000;
            delayTime = delayTime > 5 ? 5:delayTime;
            SDL_Delay(delayTime);
            iAlldelayTime += delayTime;
        }
        if (got_picture){
            pFrame->width = iwidth;
            pFrame->pts = av_frame_get_best_effort_timestamp(pFrame);
            /* push the decoded frame into the filtergraph */
            /*if (av_buffersrc_add_frame(m_pstVideosState->player->buffersrc_ctx, pFrame) < 0)
            {
                printf( "Error while feeding the filtergraph\n");
                break;
            }*/
            /*ret = av_buffersink_get_frame(m_pstVideosState->player->buffersink_ctx, pFrame);
            if (ret < 0)
            {
                break;
            }*/
            sws_scale(img_convert_ctx,
                    (uint8_t const * const *) pFrame->data,
                    pFrame->linesize, 0, iheight, pFrameRGB->data,
                    pFrameRGB->linesize);

            /**把这个RGB数据 用QImage加载**/
            QImage tmpImg((uchar *)out_buffer_rgb,iwidth,iheight,QImage::Format_RGB32);
             /**把图像复制一份 传递给界面显示**/
            QImage image = tmpImg.copy();
            /**调用激发信号的函数，更新图像显示**/
            //emit sig_GetOneFrame(image);
            m_pstVideosState->player->disPlayVideo(image);
        }
        av_free_packet(packet);
    }
    av_free(pFrame);
    av_free(pFrameRGB);
    av_free(out_buffer_rgb);
    if (!m_pstVideosState->quit)
    {
        m_pstVideosState->quit = true;
    }
    m_pstVideosState->videoThreadFinished = true;
    qDebug()<<"video_thread end\t"<< m_pstVideosState->player->PrintCurrentTime();
    return 0;
}



VideoPlayer::VideoPlayer()
{
    mPlayerState = Stop;
}

VideoPlayer::~VideoPlayer()
{

}

void VideoPlayer::disPlayVideo(QImage img)
{
    emit sig_GetOneFrame(img);
}

void VideoPlayer::StartPlay()
{
    mPlayerState = Playing;
    this->start();
}

void VideoPlayer::run()
{
    AVCodec *l_pstVCodec;//视频解码器
    AVCodec *l_pstACodec;//音频解码器
    AVFormatContext *l_pstFormatCtx;//存储整个视频信息
    AVCodecContext *l_pstVCodecCtx;//视频解码器
    AVCodecContext *l_pstACodecCtx;//音频解码器
    VideoState *m_pstVideosState = &mVideoState;

    int l_s32VStreamCount;
    int l_s32AStreamCount;
    char file_path[1280] = {0};
    baudio = true;
    m_benterThread = false;//记录是否进入了解码线程
    strcpy(file_path,mFileName.toUtf8().data());
    memset(&mVideoState,0,sizeof(VideoState));
    qDebug()<<("run start：") << mFileName;
    av_register_all();
    /**SDL初始化**/
    if (SDL_Init(SDL_INIT_AUDIO)){
        fprintf(stderr,"Could not initialize SDL - %s. \n", SDL_GetError());
        exit(1);
    }
    /**分配空间**/
    l_pstFormatCtx = avformat_alloc_context();

    /**打开输入文件**/
    if (avformat_open_input(&l_pstFormatCtx, file_path, NULL, NULL) != 0){
        printf("can't open the file. \n");
        return;
    }/**获取媒体信息**/
    if (avformat_find_stream_info(l_pstFormatCtx, NULL) < 0){
        printf("Could't find stream infomation.\n");
        return;
    }
    l_s32VStreamCount = -1;
    l_s32AStreamCount = -1;
    /**循环查找视频中包含的流信息**/
    for (uint i = 0; i < l_pstFormatCtx->nb_streams; i++){
        if (l_pstFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            l_s32VStreamCount = i;//记录视频流的数量
        if (l_pstFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO  && l_s32AStreamCount < 0)
            l_s32AStreamCount = i;//记录音频流的数量
    }
    /**赋值**/
    m_pstVideosState->pFormatContext = l_pstFormatCtx;
    m_pstVideosState->s32VStreamCount = l_s32VStreamCount;
    m_pstVideosState->s32AStreamCount = l_s32AStreamCount;
    /**发送媒体长度，用来更新显示进度条**/
    if (l_s32AStreamCount >= 0) {
        /**所有设置SDL音频流信息的步骤都在这个函数里完成**/
        audio_stream_component_open(&mVideoState, l_s32AStreamCount);
    }
    /**查找音频解码器**/
    l_pstACodecCtx = l_pstFormatCtx->streams[l_s32AStreamCount]->codec;
    l_pstACodec = avcodec_find_decoder(l_pstACodecCtx->codec_id);
    if (l_pstACodec == NULL) {
        printf("ACodec not found.\n");
        return;
    }
    /**打开音频解码器**/
    if (avcodec_open2(l_pstACodecCtx, l_pstACodec, NULL) < 0) {
        printf("Could not open audio codec.\n");
        return;
    }
    m_pstVideosState->player = this;
    //转化为音频流
    m_pstVideosState->audio_st = l_pstFormatCtx->streams[l_s32AStreamCount];
    if (l_s32VStreamCount >= 0){
        /**查找视频解码器**/
        l_pstVCodecCtx = l_pstFormatCtx->streams[l_s32VStreamCount]->codec;
        l_pstVCodec = avcodec_find_decoder(l_pstVCodecCtx->codec_id);

        if (l_pstVCodec == NULL) {
            printf("PCodec not found.\n");
            return;
        }
        l_pstVCodecCtx->bit_rate =0;   //初始化为0
        l_pstVCodecCtx->time_base.num=1;  //下面两行：一秒钟25帧
        l_pstVCodecCtx->time_base.den=10;
        l_pstVCodecCtx->frame_number=1;  //每包一个视频帧
        /**打开视频解码器**/
        if (avcodec_open2(l_pstVCodecCtx, l_pstVCodec, NULL) < 0) {
            printf("Could not open video codec.\n");
            return;
        }
        //转化为视频流
        m_pstVideosState->video_st = l_pstFormatCtx->streams[l_s32VStreamCount];
        /**初始化队列**/
        packet_queue_init(&m_pstVideosState->videoq);
        /**创建一个线程专门用来解码视频**/
        m_benterThread = true;
        m_pstVideosState->video_tid = SDL_CreateThread(video_thread, "video_thread", &mVideoState);
    }
    /**分配一个packet 用来存放读取的视频**/
    AVPacket *packet = (AVPacket *) malloc(sizeof(AVPacket));
    /**打印输出视频信息**/
    av_dump_format(l_pstFormatCtx, 0, file_path, 0);

    while(1){
        /**停止播放了**/
        if (m_pstVideosState->quit){
            qDebug()<<"run quit\t"<<PrintCurrentTime();
            break;
        }
        /**这里做了个限制  当队列里面的数据超过某个大小的时候 就暂停读取  防止一下子就把视频读完了，导致的空间分配不足**/
        /**这里audioq.size是指队列中的所有数据包带的音频数据的总量或者视频数据总量，并不是包的数量**/
        /**这个值可以稍微写大一些**/
        if (m_pstVideosState->audioq.size > MAX_AUDIO_SIZE || m_pstVideosState->videoq.size > MAX_VIDEO_SIZE)
        {
            SDL_Delay(10);
            continue;
        }
        if (av_read_frame(l_pstFormatCtx, packet) < 0){
            m_pstVideosState->readFinished = true;

            if (m_pstVideosState->quit)
            {
                break; /**解码线程也执行完了 可以退出了**/
            }
            SDL_Delay(10);
            continue;
        }
        if (packet->stream_index == l_s32VStreamCount){
            /**将视频数据存入视频队列中**/
            packet_queue_put(&m_pstVideosState->videoq, packet);
            //qDebug()<<("video input num %d \n",l_s32VideoPacketNum++);
        }
        else if( packet->stream_index == l_s32AStreamCount ){
            /**将音频数据存入音频队列中**/
            packet_queue_put(&m_pstVideosState->audioq, packet);
            //qDebug()<<("audio input num %d \n",l_s32AudioPacketNum++);
        }
        else{
            /**Free the packet that was allocated by av_read_frame**/
            av_free_packet(packet);
        }
    }
    /**文件读取结束 跳出循环的情况**/
    /**等待播放完毕**/
    //必须先等当前的子线程，运行完了以后再退出当前线程
    while ((!m_pstVideosState->videoThreadFinished) && (l_s32VStreamCount >= 0)){
        qDebug()<<"run wait videoThreadFinished\t"<<PrintCurrentTime();
        SDL_Delay(10);
    }
    qDebug()<<"video run exit\t"<< m_pstVideosState->player->PrintCurrentTime();
    SDL_WaitThread(mVideoState.video_tid,NULL);
    if(l_s32VStreamCount >= 0)
    {
       avcodec_close(l_pstVCodecCtx);
    }
    avformat_close_input(&l_pstFormatCtx);
    m_pstVideosState->readThreadFinished = true;
    SDL_CloseAudioDevice(m_pstVideosState->audioID);
    //stop();
    //如果是正常播放完成则进行下一首的播放
    if(bPlayend){
        SDL_Delay(100);
        qDebug()<<"bPlayend quit\t"<< PrintCurrentTime();
        //emit Playend();
    }
}

QString VideoPlayer::PrintCurrentTime()
{
    QDateTime current_date_time = QDateTime::currentDateTime();
    //QString current_date = current_date_time.toString("yyyy-MM-dd");
    return current_date_time.toString("hh:mm:ss.zzz ");
}

bool VideoPlayer::setFileName(QString path)
{
    if (mPlayerState != Stop)
        return false;

    mFileName = path;
    mPlayerState = Playing;

    return true;
}
