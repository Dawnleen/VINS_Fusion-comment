/*******************************************************

 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology

 * 

 * This file is part of VINS.

 * 

 * Licensed under the GNU General Public License v3.0;

 * you may not use this file except in compliance with the License.

 *

 * Author: Qin Tong (qintonguav@gmail.com)

 *******************************************************/



#include <stdio.h>

#include <queue>

#include <map>

#include <thread>

#include <mutex>

#include <ros/ros.h>

#include <cv_bridge/cv_bridge.h>

#include <opencv2/opencv.hpp>

#include "estimator/estimator.h"

#include "estimator/parameters.h"

#include "utility/visualization.h"



Estimator estimator;



queue<sensor_msgs::ImuConstPtr> imu_buf;

queue<sensor_msgs::PointCloudConstPtr> feature_buf;//存储feature

queue<sensor_msgs::ImageConstPtr> img0_buf;//存储image0 左侧图像

queue<sensor_msgs::ImageConstPtr> img1_buf;//存储image1 左侧图像

std::mutex m_buf;





void img0_callback(const sensor_msgs::ImageConstPtr &img_msg)

{

    m_buf.lock();

    img0_buf.push(img_msg);

    m_buf.unlock();

}



void img1_callback(const sensor_msgs::ImageConstPtr &img_msg)

{

    m_buf.lock();

    img1_buf.push(img_msg);

    m_buf.unlock();

}





//转为mat类型

cv::Mat getImageFromMsg(const sensor_msgs::ImageConstPtr &img_msg)

{

    cv_bridge::CvImageConstPtr ptr;

    if (img_msg->encoding == "8UC1")

    {

        sensor_msgs::Image img;

        img.header = img_msg->header;

        img.height = img_msg->height;

        img.width = img_msg->width;

        img.is_bigendian = img_msg->is_bigendian;

        img.step = img_msg->step;

        img.data = img_msg->data;

        img.encoding = "mono8";

        ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO8);

    }

    else

        ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);



    cv::Mat img = ptr->image.clone();

    return img;

}



// extract images with same timestamp from two topics

//能够将同样时间戳的两帧图片同时放入estimator中

void sync_process()

{

    while(1)

    {

        //STEREO 取值为1 表示双目

        if(STEREO)

        {

            cv::Mat image0, image1;

            std_msgs::Header header;

            double time = 0;

            m_buf.lock();

            if (!img0_buf.empty() && !img1_buf.empty())

            {

                double time0 = img0_buf.front()->header.stamp.toSec();

                double time1 = img1_buf.front()->header.stamp.toSec();

                // 0.003s sync tolerance 保证图像对应

                if(time0 < time1 - 0.003)

                {

                    img0_buf.pop();

                    printf("throw img0\n");

                }

                else if(time0 > time1 + 0.003)

                {

                    img1_buf.pop();

                    printf("throw img1\n");

                }

                else

                {

                    time = img0_buf.front()->header.stamp.toSec();

                    header = img0_buf.front()->header;

                    image0 = getImageFromMsg(img0_buf.front()); //转为mat类型


                    img0_buf.pop();

                    image1 = getImageFromMsg(img1_buf.front()); //转为mat类型

                    img1_buf.pop();

                    //printf("find img0 and img1\n");

                }

            }

            m_buf.unlock();

            if(!image0.empty())

                estimator.inputImage(time, image0, image1);//给featureTracker.trackImage输入图像

        }

        else

        {

            cv::Mat image;

            std_msgs::Header header;

            double time = 0;

            m_buf.lock();

            if(!img0_buf.empty())

            {

                time = img0_buf.front()->header.stamp.toSec();

                header = img0_buf.front()->header;

                image = getImageFromMsg(img0_buf.front());

                img0_buf.pop();

            }

            m_buf.unlock();

            if(!image.empty())

                estimator.inputImage(time, image);

        }



        std::chrono::milliseconds dura(2);

        std::this_thread::sleep_for(dura);

    }

}



//接受imu传感器数据，并计算里程计数据

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)

{

    double t = imu_msg->header.stamp.toSec();

    double dx = imu_msg->linear_acceleration.x;

    double dy = imu_msg->linear_acceleration.y;

    double dz = imu_msg->linear_acceleration.z;

    double rx = imu_msg->angular_velocity.x;

    double ry = imu_msg->angular_velocity.y;

    double rz = imu_msg->angular_velocity.z;

    //  线速度

    Vector3d acc(dx, dy, dz);

    //  角速度

    Vector3d gyr(rx, ry, rz);

    estimator.inputIMU(t, acc, gyr);

    return;

}



//跟踪光流点

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)

{

    map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> featureFrame;

    for (unsigned int i = 0; i < feature_msg->points.size(); i++)

    {

        int feature_id = feature_msg->channels[0].values[i];

        int camera_id = feature_msg->channels[1].values[i];

        double x = feature_msg->points[i].x;

        double y = feature_msg->points[i].y;

        double z = feature_msg->points[i].z;

        double p_u = feature_msg->channels[2].values[i];

        double p_v = feature_msg->channels[3].values[i];

        double velocity_x = feature_msg->channels[4].values[i];

        double velocity_y = feature_msg->channels[5].values[i];

        if(feature_msg->channels.size() > 5)

        {

            double gx = feature_msg->channels[6].values[i];

            double gy = feature_msg->channels[7].values[i];

            double gz = feature_msg->channels[8].values[i];

            pts_gt[feature_id] = Eigen::Vector3d(gx, gy, gz);

            //printf("receive pts gt %d %f %f %f\n", feature_id, gx, gy, gz);

        }

        ROS_ASSERT(z == 1);

        Eigen::Matrix<double, 7, 1> xyz_uv_velocity;

        xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;

        featureFrame[feature_id].emplace_back(camera_id,  xyz_uv_velocity);

    }

    double t = feature_msg->header.stamp.toSec();

    estimator.inputFeature(t, featureFrame);

    return;

}



void restart_callback(const std_msgs::BoolConstPtr &restart_msg)

{

    if (restart_msg->data == true)

    {

        ROS_WARN("restart the estimator!");

        estimator.clearState();//清除状态 赋初始值

        estimator.setParameter();//根据配置文件赋值

    }

    return;

}



//

void imu_switch_callback(const std_msgs::BoolConstPtr &switch_msg)

{

    if (switch_msg->data == true)

    {

        //ROS_WARN("use IMU!");

        estimator.changeSensorType(1, STEREO);//是否使用imu，如果没有重新初始化

    }

    else

    {

        //ROS_WARN("disable IMU!");

        estimator.changeSensorType(0, STEREO);

    }

    return;

}





//判断使用了立体声，还是单目

void cam_switch_callback(const std_msgs::BoolConstPtr &switch_msg)

{

    if (switch_msg->data == true)

    {

        //ROS_WARN("use stereo!");

        estimator.changeSensorType(USE_IMU, 1);

    }

    else

    {

        //ROS_WARN("use mono camera (left)!");

        estimator.changeSensorType(USE_IMU, 0);

    }

    return;

}



int main(int argc, char **argv)

{

    ros::init(argc, argv, "vins_estimator"); //初始化

    ros::NodeHandle n("~"); //句柄

    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info); //设置日志级别



    if(argc != 2)

    {

        printf("please intput: rosrun vins vins_node [config file] \n"

               "for example: rosrun vins vins_node "

               "~/catkin_ws/src/VINS-Fusion/config/euroc/euroc_stereo_imu_config.yaml \n");

        return 1;

    }



    string config_file = argv[1];

    printf("config_file: %s\n", argv[1]);



    //读取yaml内的参数

    readParameters(config_file);

    //开启了滑动窗口估计的一个新线程，读取参数并配置参数

    estimator.setParameter();



#ifdef EIGEN_DONT_PARALLELIZE

    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");

#endif



    ROS_WARN("waiting for image and imu...");



    registerPub(n);



    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());

    ros::Subscriber sub_feature = n.subscribe("/feature_tracker/feature", 2000, feature_callback);//源数据为特征点

    ros::Subscriber sub_img0 = n.subscribe(IMAGE0_TOPIC, 100, img0_callback);//左侧图像

    ros::Subscriber sub_img1 = n.subscribe(IMAGE1_TOPIC, 100, img1_callback);//右侧图像

    ros::Subscriber sub_restart = n.subscribe("/vins_restart", 100, restart_callback);//是否重新初始化

    ros::Subscriber sub_imu_switch = n.subscribe("/vins_imu_switch", 100, imu_switch_callback);

    ros::Subscriber sub_cam_switch = n.subscribe("/vins_cam_switch", 100, cam_switch_callback);



    std::thread sync_thread{sync_process};//入口

    ros::spin();



    return 0;

}
