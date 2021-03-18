#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "videoplayer.h"
#include <QImage>
#include <QPaintEvent>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    static bool m_bStop;//记录死否点击了停止按钮

protected:
    void paintEvent(QPaintEvent *event);

private:
    Ui::MainWindow *ui;
    VideoPlayer *mPlayer; //播放线程
    QImage mImage; //记录当前的图像
    QTimer *mTimer = nullptr; /**定时器-获取当前视频时间**/

private slots:
    void slotGetOneFrame(QImage img);
};
#endif // MAINWINDOW_H
