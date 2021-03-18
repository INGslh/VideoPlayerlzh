#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H
#include <QThread>
#include <QImage>
#include <QMutex>

extern "C"
{
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
    #include "libavutil/time.h"
    #include "libavutil/pixfmt.h"
    #include "libswscale/swscale.h"
    #include "libavfilter/avfilter.h"
    #include "libavfilter/buffersink.h"
    #include "libavfilter/buffersrc.h"
    #include "libswresample/swresample.h"
    #include "libavutil/imgutils.h"

    #include <SDL/include/SDL.h>
    #include <SDL/include/SDL_audio.h>
    #include <SDL/include/SDL_types.h>
    #include <SDL/include/SDL_name.h>
    #include <SDL/include/SDL_main.h>
    #include <SDL/include/SDL_config.h>
}

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define MAX_AUDIO_SIZE (25 * 16 * 1024)
#define MAX_VIDEO_SIZE (25 * 256 * 1024)
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 /**1 second of 48khz 32bit audio**/

class VideoPlayer; /**前置声明**/

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 1
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000 // 1 second of 48khz 32bit audio

typedef struct VideoState {
    DECLARE_ALIGNED(16,uint8_t,audio_buf2) [AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];//字节对齐，提升效率
    AVFormatContext *pFormatContext; /**文件格式上下文**/
    PacketQueue audioq;//音频队列
    AVFrame *audio_frame;/**解码音频过程中的使用缓存**/
    AVStream *audio_st; /**音频流**/
    AVStream *video_st;//视频流
    AVPacket audio_pkt;
    PacketQueue videoq;//视频队列

    struct SwrContext *swr_ctx; /**用于解码后的音频格式转换**/

    uint8_t *audio_buf;
    uint8_t *audio_pkt_data;

    int audio_pkt_size;
    int s32VStreamCount; /**视频流数量**/
    int s32AStreamCount; /**音频流数量**/

    int audio_src_channels;
    int audio_tgt_channels;
    int audio_src_freq;
    int audio_tgt_freq;
    int audio_hw_buf_size;

    unsigned int audio_buf_size;
    unsigned int audio_buf_index;

    int64_t audio_src_channel_layout;
    int64_t audio_tgt_channel_layout;

    enum AVSampleFormat audio_src_fmt;
    enum AVSampleFormat audio_tgt_fmt;

    double audio_clock; /**音频时钟**/
    double video_clock; /**<pts of last decoded frame / predicted pts of next decoded frame**/

    /**跳转相关的变量**/
    int             seek_req; /**跳转标志**/
    int64_t         seek_pos; /**跳转的位置 -- 微秒**/
    int             seek_flag_audio;/**跳转标志 -- 用于音频线程中**/
    int             seek_flag_video;/**跳转标志 -- 用于视频线程中**/
    double          seek_time;      /**跳转的时间(秒)  值和seek_pos是一样的**/

    /**播放控制相关**/
    bool isPause;       /**暂停标志**/
    bool quit;          /**停止**/
    bool readFinished;  /**文件读取完毕**/
    bool readThreadFinished;
    bool videoThreadFinished;

    SDL_Thread *video_tid;       /**视频线程id**/
    SDL_AudioDeviceID audioID;   /**SDL Audio Device IDs.**/
    VideoPlayer *player;         /**记录下这个类的指针  主要用于在线程里面调用激发信号的函数**/

} VideoState;

class VideoPlayer : public QThread
{
    Q_OBJECT
public:

    enum PlayerState
    {
        Playing,
        Pause,
        Stop
    };

    VideoPlayer();
    ~VideoPlayer();

    bool setFileName(QString path);

    void StartPlay();
    //bool pause();
    //bool Setvol(double value);
    //bool stop(bool isWait = false); //参数表示是否等待所有的线程执行完毕再返回
    //void seek(int64_t pos); //单位是微秒

    //int64_t getTotalTime(); //单位微秒
    //double getCurrentTime(); //单位秒
    void disPlayVideo(QImage img);
    //void disPlayVideo(uchar *ptr,uint width,uint height);
    //int SaveYuv(unsigned char *buf, int wrap, int xsize, int ysize, char *filename);
    QString PrintCurrentTime();
    //VideoState getvideoState();
    //PlayerState getPlayerState();
    bool bPlayend = false;//判断是否是正常播放完成
    bool baudio = false;//判断音频解码是否成功
    bool m_benterThread = false;
    double m_vol = 1.0;//存储音量
    AVFilterGraph * m_filter_graph;
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    //void SetVolume(uint8_t *buf, uint32_t size, uint32_t uRepeat, double vol);
    PlayerState mPlayerState; //播放状态
    //int init_filters(AVCodecContext *pCodecCtx, const char *filters_descr);
signals:
    void sig_GetOneFrame(QImage); //每获取到一帧图像 就发送此信号

protected:
    void run();

private:
    QString mFileName;
    VideoState mVideoState; //用来传递给 SDL音频回调函数的数据
};

#endif // VIDEOPLAYER_H
