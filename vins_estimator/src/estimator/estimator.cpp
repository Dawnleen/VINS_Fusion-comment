/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 * 
 * This file is part of VINS.
 * 
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *******************************************************/

#include "estimator.h"
#include "../utility/visualization.h"

Estimator::Estimator(): f_manager{Rs}
{
    ROS_INFO("init begins");
    initThreadFlag = false;
    clearState();
}

Estimator::~Estimator()
{
    if (MULTIPLE_THREAD)
    {
        processThread.join();  //TODO(tzhang):等待该线程结束,但该线程的写法有问题，processMeasurements为死循环，不会自己结束，此处可完善修改
        printf("join thread \n");
    }
}
//清除了状态，为诸多变量赋初值
void Estimator::clearState()
{
    mProcess.lock();
    while(!accBuf.empty())
        accBuf.pop();
    while(!gyrBuf.empty())
        gyrBuf.pop();
    while(!featureBuf.empty())
        featureBuf.pop();

    prevTime = -1;
    curTime = 0;
    openExEstimation = 0;
    initP = Eigen::Vector3d(0, 0, 0);
    initR = Eigen::Matrix3d::Identity();
    inputImageCnt = 0;
    initFirstPoseFlag = false;

    for (int i = 0; i < WINDOW_SIZE + 1; i++)
    {
        Rs[i].setIdentity();
        Ps[i].setZero();
        Vs[i].setZero();
        Bas[i].setZero();
        Bgs[i].setZero();
        dt_buf[i].clear();
        linear_acceleration_buf[i].clear();
        angular_velocity_buf[i].clear();

        if (pre_integrations[i] != nullptr)
        {
            delete pre_integrations[i];
        }
        pre_integrations[i] = nullptr;
    }

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        tic[i] = Vector3d::Zero();
        ric[i] = Matrix3d::Identity();
    }

    first_imu = false,
    sum_of_back = 0;
    sum_of_front = 0;
    frame_count = 0;
    solver_flag = INITIAL;
    initial_timestamp = 0;
    all_image_frame.clear();

    if (tmp_pre_integration != nullptr)
        delete tmp_pre_integration;
    if (last_marginalization_info != nullptr)
        delete last_marginalization_info;

    tmp_pre_integration = nullptr;
    last_marginalization_info = nullptr;
    last_marginalization_parameter_blocks.clear();

    f_manager.clearState();

    failure_occur = 0;

    mProcess.unlock();
}

void Estimator::setParameter()
{
    mProcess.lock();  // 锁mProcess主要用于processThread线程互斥安全
    for (int i = 0; i < NUM_OF_CAM; i++)  //设置左右相机与IMU（或体坐标系）之间的变换矩阵
    {
        tic[i] = TIC[i];
        ric[i] = RIC[i];
        cout << " exitrinsic cam " << i << endl  << ric[i] << endl << tic[i].transpose() << endl;
    }
    f_manager.setRic(ric);  //以下设置图像因子需使用的sqrt_info（即信息矩阵的根号值），该设置假定图像特征点提取存在1.5个像素的误差，
                            // 存在FOCAL_LENGTH,是因为图像因子残差评估在归一化相机坐标系下进行
    ProjectionTwoFrameOneCamFactor::sqrt_info = FOCAL_LENGTH / 1.5 * Matrix2d::Identity();
    ProjectionTwoFrameTwoCamFactor::sqrt_info = FOCAL_LENGTH / 1.5 * Matrix2d::Identity();
    ProjectionOneFrameTwoCamFactor::sqrt_info = FOCAL_LENGTH / 1.5 * Matrix2d::Identity();
    td = TD;
    g = G;
    cout << "set g " << g.transpose() << endl;
    featureTracker.readIntrinsicParameter(CAM_NAMES);  // 设置相机内参，该参数主要用于特征点跟踪过程

    std::cout << "MULTIPLE_THREAD is " << MULTIPLE_THREAD << '\n';
    if (MULTIPLE_THREAD && !initThreadFlag)
    {
        initThreadFlag = true;  //根据配置文件，创建新的线程，该线程的回调函数为processMeasurements
        processThread = std::thread(&Estimator::processMeasurements, this);
    }
    mProcess.unlock();
}

//是否使用imu，如果没有重新初始化
void Estimator::changeSensorType(int use_imu, int use_stereo)
{
    bool restart = false;
    mProcess.lock();
    if(!use_imu && !use_stereo)
        printf("at least use two sensors! \n");
    else
    {
        if(USE_IMU != use_imu)
        {
            USE_IMU = use_imu;
            if(USE_IMU)
            {
                // reuse imu; restart system
                restart = true;
            }
            else
            {
                if (last_marginalization_info != nullptr)
                    delete last_marginalization_info;

                tmp_pre_integration = nullptr;
                last_marginalization_info = nullptr;
                last_marginalization_parameter_blocks.clear();
            }
        }
        
        STEREO = use_stereo;
        printf("use imu %d use stereo %d\n", USE_IMU, STEREO);
    }
    mProcess.unlock();
    if(restart)  //更改传感器配置后，vio系统重启，此处重启与检测到failure后的重启一样，主要包括清除状态、设置参数两步
    {
        clearState();
        setParameter();
    }
}

//给featureTracker.trackImage输入图像
void Estimator::inputImage(double t, const cv::Mat &_img, const cv::Mat &_img1)
{
    inputImageCnt++;
    map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> featureFrame;   // feature_id  camera_id  x, y, z, p_u, p_v, velocity_x, velocity_y;
    TicToc featureTrackerTime;
    
    //是否双目
    if(_img1.empty()) //只有左相机图像
        featureFrame = featureTracker.trackImage(t, _img);
    else
        featureFrame = featureTracker.trackImage(t, _img, _img1);
    //printf("featureTracker time: %f\n", featureTrackerTime.toc());

    if (SHOW_TRACK)
    {
        cv::Mat imgTrack = featureTracker.getTrackImage();
        //将时间与图像一起发送，把追踪的图片imgTrack发布出去.
        pubTrackImage(imgTrack, t);
    }
    
    if(MULTIPLE_THREAD)  
    {   //多线程时，在函数setParameter中，processMeasurements已经作为回调函数、创建了新的线程processThread
        if(inputImageCnt % 2 == 0)  //TODO(tzhang):多线程的时候，仅使用偶数计数的图像特征？如果计算量足够或者图像帧率较低，可去除此处限制 tzhang
        {
            mBuf.lock();
            featureBuf.push(make_pair(t, featureFrame));
            mBuf.unlock();
        }
    }
    else
    {
        mBuf.lock();
        featureBuf.push(make_pair(t, featureFrame));
        mBuf.unlock();
        TicToc processTime;
        processMeasurements();
        printf("process time: %f\n", processTime.toc());
    }
    
}

void Estimator::inputIMU(double t, const Vector3d &linearAcceleration, const Vector3d &angularVelocity)
{
    //P是位置，Q是四元数字，V是速度
    mBuf.lock();
    accBuf.push(make_pair(t, linearAcceleration));
    gyrBuf.push(make_pair(t, angularVelocity));
    //printf("input imu with time %f \n", t);
    mBuf.unlock();

    if (solver_flag == NON_LINEAR)
    {
        mPropagate.lock();
        fastPredictIMU(t, linearAcceleration, angularVelocity);  //IMU 中值积分预测，计算最新p、v、q
        pubLatestOdometry(latest_P, latest_Q, latest_V, t);  // 发布中值积分预测的状态（最新数据），主要是p、v、q
        mPropagate.unlock();
    }
}

void Estimator::inputFeature(double t, const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &featureFrame)  //该函数并未使用
{
    mBuf.lock();
    featureBuf.push(make_pair(t, featureFrame));
    mBuf.unlock();

    if(!MULTIPLE_THREAD)
        processMeasurements();
}

//获取preTime以及curTime这两个时间段之间IMU数据，存放到accVector以及gyrVector里面
bool Estimator::getIMUInterval(double t0, double t1, vector<pair<double, Eigen::Vector3d>> &accVector, 
                                vector<pair<double, Eigen::Vector3d>> &gyrVector)  //获取t0和t1时间间隔内的加速度和陀螺仪数据，t0和t1一般为相邻图像帧时间戳
{
    if(accBuf.empty())
    {
        printf("not receive imu\n");
        return false;
    }
    //printf("get imu from %f %f\n", t0, t1);
    //printf("imu fornt time %f   imu end time %f\n", accBuf.front().first, accBuf.back().first);
    if(t1 <= accBuf.back().first)
    {
        while (accBuf.front().first <= t0)
        {
            accBuf.pop();
            gyrBuf.pop();
        }
        while (accBuf.front().first < t1)
        {
            accVector.push_back(accBuf.front());
            accBuf.pop();
            gyrVector.push_back(gyrBuf.front());
            gyrBuf.pop();
        }
        accVector.push_back(accBuf.front());
        gyrVector.push_back(gyrBuf.front());
    }
    else
    {
        printf("wait for imu\n");
        return false;
    }
    return true;
}

//判断输入的时间t时候的imu是否可用
bool Estimator::IMUAvailable(double t)
{
    if(!accBuf.empty() && t <= accBuf.back().first)  // 加速度vector不为空，并且图像帧时间戳不大于加速度时间戳，则认为IMU数据可用
        return true;
    else
        return false;
}

/**处理各种buffer里面的东西，IMU预积分，特征点的处理
 * 当featureBuf不为空的时候，函数开始进行前端的测量处理。
    td:代表相机与imu之间的时间差。
 * 等待imu有收集到最新图像时刻的值。
 * 接着通过getIMUInterval（）获取preTime以及curTime这两个时间段之间IMU数据，存放到accVector以及gyrVector里面。
 然后对每一帧IMU之间的时间差td,然后把加速度计以及陀螺仪的数据交给processIMU()函数处理。
 * */
void Estimator::processMeasurements()  //传感器数据处理入口，也是多线程配置中传入的回调函数
{
    while (1)
    {
        //printf("process measurments\n");
        pair<double, map<int, vector<pair<int, Eigen::Matrix<double, 7, 1> > > > > feature; //时间戳（double）、路标点编号（int）、相机编号（int）
                                        //特征点信息（Matrix<double, 7, 1>，归一化相机坐标系坐标（3维）、去畸变图像坐标系坐标（2维）、特征点速度（2维））
        vector<pair<double, Eigen::Vector3d>> accVector, gyrVector; //线速度、角速度
        if(!featureBuf.empty())
        {
            feature = featureBuf.front();
            curTime = feature.first + td;  //时间偏差补偿后的图像帧时间戳
            while(1)  // 循环等待IMU数据，IMU数据可用会break该while循环
            {
                if ((!USE_IMU  || IMUAvailable(feature.first + td)))
                    break;
                else
                {
                    printf("wait for imu ... \n");
                    if (! MULTIPLE_THREAD)
                        return;
                    std::chrono::milliseconds dura(5);
                    std::this_thread::sleep_for(dura);
                }
            }
            mBuf.lock();
            if(USE_IMU)
                getIMUInterval(prevTime, curTime, accVector, gyrVector);  //获取两图像帧时间戳之间的加速度和陀螺仪数据

            featureBuf.pop();
            mBuf.unlock();

            if(USE_IMU)
            {
                if(!initFirstPoseFlag)  //位姿未初始化，则利用加速度初始化Rs[0]
                    initFirstIMUPose(accVector);  //基于重力，对准第一帧，即将初始姿态对准到重力加速度方向
                for(size_t i = 0; i < accVector.size(); i++)
                {
                    double dt;
                    if(i == 0)
                        dt = accVector[i].first - prevTime;
                    else if (i == accVector.size() - 1)
                        dt = curTime - accVector[i - 1].first;
                    else
                        dt = accVector[i].first - accVector[i - 1].first;
                    //对IMU预积分
                    processIMU(accVector[i].first, dt, accVector[i].second, gyrVector[i].second);  //IMU数据处理，主要创建预积分因子、中值积分预测状态
                }
            }
            mProcess.lock();
            processImage(feature.second, feature.first);  //函数名字不太妥当，后续优化等过程全在该函数实现;优化等过程的入口
            prevTime = curTime;

            printStatistics(*this, 0);  //打印调试信息; 优化后的外参输出也在该函数实现

            std_msgs::Header header;
            header.frame_id = "world";
            header.stamp = ros::Time(feature.first);

            pubOdometry(*this, header);  //发布优化后的信息
            pubKeyPoses(*this, header);
            pubCameraPose(*this, header);
            pubPointCloud(*this, header);
            pubKeyframe(*this);  //TODO(tzhang): 当MARGIN_OLD时，也即次新帧为关键帧; 将次新帧发布出去，但是前面关键帧判断条件较为宽松;可将关键帧选取更严格
            pubTF(*this, header);
            mProcess.unlock();
        }

        if (! MULTIPLE_THREAD)
            break;

        std::chrono::milliseconds dura(2);
        std::this_thread::sleep_for(dura);
    }
}

//求一个姿态角,然后把航向角设为0，初始化当前一段时间内的加速度计的数据
void Estimator::initFirstIMUPose(vector<pair<double, Eigen::Vector3d>> &accVector)  //利用重力信息，初始化最开始状态中的旋转矩阵
{
    printf("init first imu pose\n");
    initFirstPoseFlag = true;
    //return;
    Eigen::Vector3d averAcc(0, 0, 0);
    int n = (int)accVector.size();
    for(size_t i = 0; i < accVector.size(); i++)
    {
        averAcc = averAcc + accVector[i].second;
    }
    averAcc = averAcc / n;
    printf("averge acc %f %f %f\n", averAcc.x(), averAcc.y(), averAcc.z());
    Matrix3d R0 = Utility::g2R(averAcc);  // 主要利用基于重力方向，得到的roll pitch信息，由于yaw信息通过重力方向，并不能恢复出来，因此减去yaw
    double yaw = Utility::R2ypr(R0).x();
    R0 = Utility::ypr2R(Eigen::Vector3d{-yaw, 0, 0}) * R0;
    //上面三个操作是把imu坐标系与重力坐标系对齐gimu=[−9.4;−2.3;0.4]gimu​=[−9.4;−2.3;0.4]变到g=[0;0;9.8]g=[0;0;9.8], 并且保证坐标系中yaw的值为0
    Rs[0] = R0;  //R0存入滑动窗口的世界坐标系下的旋转Rs[0]
    cout << "init R0 " << endl << Rs[0] << endl;
    //Vs[0] = Vector3d(5, 0, 0);
}

void Estimator::initFirstPose(Eigen::Vector3d p, Eigen::Matrix3d r)
{
    Ps[0] = p;
    Rs[0] = r;
    initP = p;
    initR = r;
}

/**对IMU进行预积分
 * 第一次调用这个函数的时候，会把first_imu设置为true,同时acc_0,gyr_0设置成当前的一帧的acc,gyr的值。

    frame_count 代表当前处理的这一帧在滑动窗口中的第几个。取值范围是在0到WINDOW_SIZE之间。
    acc_0,gyr_0，预积分计算的是两帧图像之间积分的值，通过一帧帧imu帧的迭代求解来获得。通过对pre_integrations进行push_back（），可以迭代求解出最终两帧图像间预积分的值。
    但是要计算一个个预积分，需要知道这个预积分开头的帧当前的加速度以及角速度，以这个基准来建立的。每次使用完这个函数都会更新acc_0，gyr_0的值，即当要建立一个新的预积分的时候，这个基准直接从acc_0，gyr_0获取。

因此当滑动窗口还没有满的时候，就要new出pre_integrations[frame_count]出来。或者当slidewindow的时候，会把pre_integrations[WINDOW_SIZE]delete掉。这时候，就需要new一个新的预积分出来。

    dt_buff, linear_acceration_buf , angular_velocity_buf 三者都是frame_count里面的，即一个图像帧里面包含包含了这些东西。

同时，通过IMU的这些数据，来更新三个状态量，Ps,Vs,Rs（这个是绝对坐标系下的位姿）。这时候不是用预积分，而是用正常普通的积分并且用上中值积分。 processIMU()已经完成，接着是processImage()。
 **/
void Estimator::processIMU(double t, double dt, const Vector3d &linear_acceleration, const Vector3d &angular_velocity)
{
    if (!first_imu)  // 图像帧间的第一个imu数据
    {
        first_imu = true;
        acc_0 = linear_acceleration;
        gyr_0 = angular_velocity;
    }

    if (!pre_integrations[frame_count])  // 滑窗中之前不存在该预积分，然后新建； pre_integrations为存储预积分指针的数组
    {
        pre_integrations[frame_count] = new IntegrationBase{acc_0, gyr_0, Bas[frame_count], Bgs[frame_count]};
    }
    if (frame_count != 0)  //不是滑窗中的第一帧
    {
        //预积分。push_back进行了重载，
        pre_integrations[frame_count]->push_back(dt, linear_acceleration, angular_velocity);
        //if(solver_flag != NON_LINEAR)
        tmp_pre_integration->push_back(dt, linear_acceleration, angular_velocity);

        dt_buf[frame_count].push_back(dt);  //将图像帧之间的imu数据存储至 元素为vector的数组 tzhang
        linear_acceleration_buf[frame_count].push_back(linear_acceleration);
        angular_velocity_buf[frame_count].push_back(angular_velocity);

        //计算对应绝对坐标系下的位置等
        //Rs Ps Vs是frame_count这一个图像帧开始的预积分值,是在绝对坐标系下的.
        int j = frame_count;  //中值积分，此处与预积分中的中值积分基本相似；但此处的中值积分是以世界坐标系为基准，即更新的Rs、Ps、Vs在世界坐标系下表达 tzhang
        Vector3d un_acc_0 = Rs[j] * (acc_0 - Bas[j]) - g;  //移除了偏执的加速度
        Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - Bgs[j];  //移除了偏执的gyro
        Rs[j] *= Utility::deltaQ(un_gyr * dt).toRotationMatrix();  //等式右侧对应dq，利用0.5*theta得到；左侧Rs[j]乘以dq更新旋转 tzhang
        Vector3d un_acc_1 = Rs[j] * (linear_acceleration - Bas[j]) - g;
        Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
        Ps[j] += dt * Vs[j] + 0.5 * dt * dt * un_acc;
        Vs[j] += dt * un_acc;
    }  //此处的acc_0和gyr_0定义在estimator.h中,注意与integration_base.h中的区别；两处的作用都是为了存储之前的加速度和角速度，用于中值积分  tzhang
    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity; 
}


/**
 * 处理图像
 * 后端解算位姿的函数
 * 1。通过特征点的视差（按照vins-mono的说法，是基于imu旋转补偿后特征点的视差）来判断是否是关键帧，并对应边缘化的flag;
2。判断相机到IMU的外参(R,T)是否有校正，没校正用手眼标定法进行标定,具体体现在CalibrationExRotation里面，注意这里面只有标定旋转矩阵，
没有标定平移矩阵，按照沈老师讲座上说的，外参中主要是R对系统比较敏感，如果偏差一两度，系统很容易就蹦。
3。判断是否有进行初始化：
（1）如果初始化已经完成，则就optimization（），用ceres_solver进行滑窗内（11帧）进行非线性优化的求解。按照vinsmono论文中，
主要有三项，边缘化残差， imu残差，相机重投影残差。不过其实在代码中，又加入新的残差项，有相机Imu之间同步时间差的残差项。
另外在处理图像的这个函数中，同时有一个failureDetection(),满足一定条件下系统认为系统已经挂掉了，
例如非线性求解器中的解有大跳动，求解出相机IMU的外参矩阵或IMU偏移等等，系统挂掉就清空状态，重新初始化。
（2）如果没有初始化，则要进行相应的初始化工作。


    传入的是两个参数，第一个是图像特征的描述，第二个是图像帧获取的时间


    首先通过视差判断最新进来这一帧的视差（旋转补偿的），如果视差偏小，我们到时候边缘化帧的时候，就把滑窗里面倒数第二新的帧给边缘化掉。如果视差足够的话，我们就把滑窗里面最旧的帧给边缘化掉。
此时，我们已经有了Ps，Rs, tic,ric(imu到相机的外参)，因此，我们可以把图像中两帧有跟踪到的2D进行三角化。此时是不准确的，因为Ps，Rs都是IMU直接积分得到的值。
接着进行optimization(),进行后端的非线性优化。
优化完成后，去除图像中残差大的特征点(removeOutlier)，并且删除特征点深度算出来负数的点（removeFailures()），进行故障检测.

    last_R, last_P: 滑窗里面最新的位姿
    last_R0, last_P0: 滑窗里面最旧的位姿

最后updateLatestStates(),注意的是里面调用了fastPredictIMU(),用来预测最新P,V,Q的姿态
-latest_p,latest_q,latest_v，latest_acc_0,latest_gyr_0最新时刻的姿态。这个的作用是为了刷新姿态的输出，但是这个值的误差相对会比较大，是未经过非线性优化获取的初始值。
**/
void Estimator::processImage(const map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> &image, const double header)
{
    ROS_DEBUG("new image coming ------------------------------------------");
    ROS_DEBUG("Adding feature points %lu", image.size());
    // 检测关键帧
    if (f_manager.addFeatureCheckParallax(frame_count, image, td))  //判定当前帧（frame_count）是否为关键帧 tzhang
    {
        marginalization_flag = MARGIN_OLD;  //当前帧为关键帧，则边缘化窗口中最老的图像帧（新一帧将被作为关键帧!）
        //printf("keyframe\n");
    }
    else
    {
        marginalization_flag = MARGIN_SECOND_NEW;  //当前帧为非关键帧，则边缘化当前帧 （PS：称frame_count为当前帧，可能不是很妥当，准确来说应该判断的是次新帧） tzhang
        //printf("non-keyframe\n");
    }

    ROS_DEBUG("%s", marginalization_flag ? "Non-keyframe" : "Keyframe");
    ROS_DEBUG("Solving %d", frame_count);
    ROS_DEBUG("number of feature: %d", f_manager.getFeatureCount());
    Headers[frame_count] = header;

    ImageFrame imageframe(image, header);  //特征点信息，图像帧时间戳构成当前图像帧信息 tzhang
    imageframe.pre_integration = tmp_pre_integration;
    all_image_frame.insert(make_pair(header, imageframe));
    tmp_pre_integration = new IntegrationBase{acc_0, gyr_0, Bas[frame_count], Bgs[frame_count]};  //tmp预积分重新新建初始化 tzhang

    // 估计一个外部参,并把ESTIMATE_EXTRINSIC置1,输出ric和RIC
    if(ESTIMATE_EXTRINSIC == 2)  //2,说明估计相机与IMU之间的外参，且未给定外参初始值
    {
        ROS_INFO("calibrating extrinsic param, rotation movement is needed");
        if (frame_count != 0)
        {
            //获取frame_count - 1 与 frame_count两个图像帧匹配的特征点，特征点在归一化相机坐标系 tzhang
            //这个里边放的是新图像和上一帧
            vector<pair<Vector3d, Vector3d>> corres = f_manager.getCorresponding(frame_count - 1, frame_count);
            Matrix3d calib_ric;
            if (initial_ex_rotation.CalibrationExRotation(corres, pre_integrations[frame_count]->delta_q, calib_ric))
            {
                //对应第7讲初始化 公式（7~9），具体利用两图像帧及图像帧之间陀螺仪关联的旋转信息 tzhang
                ROS_WARN("initial extrinsic rotation calib success");
                ROS_WARN_STREAM("initial extrinsic rotation: " << endl << calib_ric);
                ric[0] = calib_ric;
                RIC[0] = calib_ric;
                ESTIMATE_EXTRINSIC = 1;  //通过上述计算获取了相机与IMU之间的外参，作为初始值，后续对该外参进行在线估计
            }
        }
    }

    if (solver_flag == INITIAL)  //进入初始化步骤 tzhang
    {
        // monocular + IMU initilization
        /**单目+IMU，采用的是视觉和IMU紧耦合的方式
         * 首先要关键帧填满滑窗，SFM求解滑窗中所有帧的位姿，和所有路标点的位置，
         * 然后和IMU预积分的值对其，求解重力方向、尺度因子、陀螺零偏及每一帧的速度。
         * **/
        if (!STEREO && USE_IMU)  //单目+IMU版本的初始化
        {
            if (frame_count == WINDOW_SIZE)  //滑窗满后才开始初始化 tzhang
            {
                bool result = false;
                if(ESTIMATE_EXTRINSIC != 2 && (header - initial_timestamp) > 0.1)  //当前图像时间戳与上一次初始化的时间戳间隔大于0.1秒、且外参存在初始值才进行初始化操作
                {
                    result = initialStructure();  //对应VINS_mono论文 V章节 视觉惯性联合初始化
                    initial_timestamp = header;  //更新初始化的时间戳
                }
                if(result)  //如果初始化成功
                {
                    optimization();   //先进行一次滑动窗口非线性优化，得到当前帧与第一帧的位姿
                    updateLatestStates();  //获取滑窗中最新帧时刻的状态，并在世界坐标系下进行中值积分；重要：初始化完成后，最新状态在inputIMU函数中发布
                    solver_flag = NON_LINEAR;
                    slideWindow(); //滑动窗口
                    ROS_INFO("Initialization finish!");
                }
                else  //滑掉这一窗
                    slideWindow();  //初始化失败也需要进行滑窗处理。若global sfm求解引起的失败，则一律移除最老的图像帧；否则，根据addFeatureCheckParallax的判断，决定移除哪一帧
            }
        }

        // stereo + IMU initilization
        //双目+imu，当帧数填满滑窗后，对IMU陀螺零偏进行了估计
        if(STEREO && USE_IMU)  //双目+IMU版本的初始化
        {
            f_manager.initFramePoseByPnP(frame_count, Ps, Rs, tic, ric);  //滑窗满之前，通过PnP求解图像帧之间的位姿，通过三角化初始化路标点（逆深度）tzhang
            f_manager.triangulate(frame_count, Ps, Rs, tic, ric);   // 双目三角化
            if (frame_count == WINDOW_SIZE)    //滑窗满后开始优化处理 tzhang
            {
                map<double, ImageFrame>::iterator frame_it;
                int i = 0;
                for (frame_it = all_image_frame.begin(); frame_it != all_image_frame.end(); frame_it++)
                {
                    frame_it->second.R = Rs[i];
                    frame_it->second.T = Ps[i];
                    i++;
                }
                solveGyroscopeBias(all_image_frame, Bgs);
                
                // 对之前预积分得到的结果进行更新。
                // 预积分的好处查看就在于你得到新的Bgs，不需要又重新再积分一遍，可以通过Bgs对位姿，速度的一阶导数，进行线性近似，得到新的Bgs求解出MU的最终结果。
                for (int i = 0; i <= WINDOW_SIZE; i++)
                {
                    pre_integrations[i]->repropagate(Vector3d::Zero(), Bgs[i]);
                }
                optimization();  //整个VIO的精华和难点
                updateLatestStates();  // 让此时刻的值都等于上一时刻的值,用来更新状态
                solver_flag = NON_LINEAR;
                slideWindow();   // 滑动窗口法,就是把前后元素交换
                ROS_INFO("Initialization finish!");
            }
        }

        // stereo only initilization
        //纯双目
        if(STEREO && !USE_IMU)  //双目版本的初始化
        {
            f_manager.initFramePoseByPnP(frame_count, Ps, Rs, tic, ric);
            f_manager.triangulate(frame_count, Ps, Rs, tic, ric);
            optimization();

            if(frame_count == WINDOW_SIZE)
            {
                optimization();
                updateLatestStates();
                solver_flag = NON_LINEAR;
                slideWindow();
                ROS_INFO("Initialization finish!");
            }
        }

        if(frame_count < WINDOW_SIZE)  //滑窗未满，下一图像帧时刻的状态量用上一图像帧时刻的状态量进行初始化
                                       //（PS：processIMU函数中，会对Ps、Vs、Rs用中值积分进行更新）
        {
            frame_count++;
            int prev_frame = frame_count - 1;
            Ps[frame_count] = Ps[prev_frame];
            Vs[frame_count] = Vs[prev_frame];
            Rs[frame_count] = Rs[prev_frame];
            Bas[frame_count] = Bas[prev_frame];
            Bgs[frame_count] = Bgs[prev_frame];
        }

    }
    else  //初始化成功，优化环节 tzhang
    {
        TicToc t_solve;
        if(!USE_IMU)  //当不存在imu时，使用pnp方法进行位姿预测;存在imu时使用imu积分进行预测
            f_manager.initFramePoseByPnP(frame_count, Ps, Rs, tic, ric);
        f_manager.triangulate(frame_count, Ps, Rs, tic, ric);  // 利用三角化方法，对未初始化的路标点进行初始化;其中，双目配置时使用双目图像进行三角化; 单目配置时，使用前后帧图像进行三角化
        optimization();
        set<int> removeIndex;
        outliersRejection(removeIndex);  //基于重投影误差，检测外点
        f_manager.removeOutlier(removeIndex);
        if (! MULTIPLE_THREAD)
        {
            featureTracker.removeOutliers(removeIndex);  //若路标点为外点，则对前端图像跟踪部分的信息进行剔除更新;主要包括prev_pts, ids， track_cnt
            predictPtsInNextFrame();  //预测路标点在下一时刻左图中的坐标，基于恒速模型
        }

        ROS_DEBUG("solver costs: %fms", t_solve.toc());

        if (failureDetection())  //默认直接返回false，具体判定失败的条件可根据具体场景修正
        {
            ROS_WARN("failure detection!");
            failure_occur = 1;
            clearState();  //清除状态、重新设置参数，相当于重新开启vio
            setParameter();
            ROS_WARN("system reboot!");
            return;
        }

        slideWindow();
        f_manager.removeFailures();  //进行故障检测.
        // prepare output of VINS
        key_poses.clear();
        for (int i = 0; i <= WINDOW_SIZE; i++)
            key_poses.push_back(Ps[i]);

        //滑窗里面最新的位姿
        last_R = Rs[WINDOW_SIZE];
        last_P = Ps[WINDOW_SIZE];
        
        //滑窗里面最旧的位姿
        last_R0 = Rs[0];
        last_P0 = Ps[0];
        updateLatestStates();  //基于中值积分，计算更新位姿
    }  
}


/**
 * 1.第一步就是要检测IMU的可观性，即IMU的激励如果不够的话，会造成尺度的不客观。
2.接着使用sfm，pnp来求解出滑窗内所有帧的姿态，此时姿态是在尺度s下面的，即没有尺度信息。（单目没有办法得到尺度信息）
3.接着visualInitialAlign(),用来做视觉与IMU之间的融合，直观上讲，把纯视觉得到的结果拉拉扯扯一下，扯到IMU的尺度上来，
并且又进行求解陀螺仪的bias，solveGyroscopeBias().同时LinearIMUAlign()求解得到尺度s，同时又进行RefineGravity(),
对重力加速度的方向的大小进行求解。最后将第一帧相机的位姿调整到与重力加速度对齐的方向上来，即第一帧相机的z轴与重力加速度平行。
这个就是代码中注释的//change state，下面做的东西
 * */
bool Estimator::initialStructure()
{
    TicToc t_sfm;
    //check imu observibility
    {//TODO(tzhang):该作用域段主要用于检测IMU运动是否充分，但是目前代码中，运动不充分时并未进行额外处理，此处可改进；或者直接删除该段
        map<double, ImageFrame>::iterator frame_it;
        Vector3d sum_g;
        for (frame_it = all_image_frame.begin(), frame_it++; frame_it != all_image_frame.end(); frame_it++)
        {//对除第一帧以外的所有图像帧数据进行遍历,计算加速度均值
            double dt = frame_it->second.pre_integration->sum_dt;
            Vector3d tmp_g = frame_it->second.pre_integration->delta_v / dt;  //通过速度计算，上一图像帧时刻到当前图像帧时刻的平均加速度
            sum_g += tmp_g;  //平均加速度累积
        }
        Vector3d aver_g;
        aver_g = sum_g * 1.0 / ((int)all_image_frame.size() - 1);  //计算所有图像帧间的平均加速度均值（注意size-1，因为N个图像帧，只有N-1个平均加速度）
        double var = 0;
        for (frame_it = all_image_frame.begin(), frame_it++; frame_it != all_image_frame.end(); frame_it++)
        {//对除第一帧以外的所有图像帧数据进行遍历,计算加速度方差
            double dt = frame_it->second.pre_integration->sum_dt;
            Vector3d tmp_g = frame_it->second.pre_integration->delta_v / dt;
            var += (tmp_g - aver_g).transpose() * (tmp_g - aver_g);
            //cout << "frame g " << tmp_g.transpose() << endl;
        }
        var = sqrt(var / ((int)all_image_frame.size() - 1));  //加速度均方差
        //ROS_WARN("IMU variation %f!", var);
        if(var < 0.25)  //加速度均方差过小，也即加速度变化不够剧烈，因此判定IMU运动不充分
        {
            ROS_INFO("IMU excitation not enouth!");
            //return false;
        }
    }
    // global sfm
    Quaterniond Q[frame_count + 1];  //R_w_c  from camera frame to world frame. tzhang
    Vector3d T[frame_count + 1];  // t_w_c
    map<int, Vector3d> sfm_tracked_points;  //观测到的路标点的在世界坐标系的位置，索引为路标点的编号
    vector<SFMFeature> sfm_f;
    for (auto &it_per_id : f_manager.feature)  //对所有路标点进行遍历
    {
        int imu_j = it_per_id.start_frame - 1;
        SFMFeature tmp_feature;
        tmp_feature.state = false;
        tmp_feature.id = it_per_id.feature_id;  //路标点的编号设置tmp_feature的ID
        for (auto &it_per_frame : it_per_id.feature_per_frame)  //对观测到路标点j的所有图像帧进行遍历
        {
            imu_j++;
            Vector3d pts_j = it_per_frame.point;
            tmp_feature.observation.push_back(make_pair(imu_j, Eigen::Vector2d{pts_j.x(), pts_j.y()}));  //构建路标点在左相机图像坐标系下的观测（去畸变）
        }
        sfm_f.push_back(tmp_feature);
    } 
    Matrix3d relative_R;
    Vector3d relative_T;
    int l;
    if (!relativePose(relative_R, relative_T, l))  //通过本质矩阵求取滑窗最后一帧（WINDOW_SIZE）到图像帧l的旋转和平移变换
    {  //共视点大于20个、视差足够大，才进行求取
        ROS_INFO("Not enough features or parallax; Move device around");
        return false;
    }
    GlobalSFM sfm;  // 通过global sfm求取滑窗中的图像帧位姿，以及观测到的路标点的位置
    if(!sfm.construct(frame_count + 1, Q, T, l,  //只有frame_count == WINDOW_SIZE才会调用initialStructure，此时frame_count即为WINDOW_SIZE
              relative_R, relative_T,
              sfm_f, sfm_tracked_points))
    {
        ROS_DEBUG("global SFM failed!");  //global sfm求解失败，对老的图像帧进行边缘化
        marginalization_flag = MARGIN_OLD;
        return false;
    }

    //solve pnp for all frame 对所有图像帧处理，并得到imu与world之间的变换
    map<double, ImageFrame>::iterator frame_it;
    map<int, Vector3d>::iterator it;
    frame_it = all_image_frame.begin( );
    for (int i = 0; frame_it != all_image_frame.end( ); frame_it++) //对所有图像帧进行遍历，i为滑窗图像帧index，frame_it为所有图像帧索引； 
                                                                    //滑窗图像帧是所有图像帧的子集,由于滑窗中可能通过MARGIN_SECOND_NEW，边缘化某些中间帧
    {
        // provide initial guess
        cv::Mat r, rvec, t, D, tmp_r;
        if((frame_it->first) == Headers[i])  //该图像帧在滑窗里面
        {
            frame_it->second.is_key_frame = true;
            frame_it->second.R = Q[i].toRotationMatrix() * RIC[0].transpose();  //得到R_w_i
            frame_it->second.T = T[i];  //TODO(tzhang):没有对imu与cam平移进行处理，是因为两者之间的平移量较小，影响不大？ 可优化
            i++;
            continue;  //若图像帧在滑窗中，直接利用上述global sfm的结果乘以R_c_i，得到imu到world的变换矩阵即R_w_i
        }
        if((frame_it->first) > Headers[i])  // 时间戳比较，仅仅在所有图像的时间戳大于等于滑窗中图像帧时间戳时，才递增滑窗中图像时间戳
        {
            i++;
        }
        Matrix3d R_inital = (Q[i].inverse()).toRotationMatrix();  //由于后续进行PnP求解，需要R_c_w与t_c_w作为初值；且以滑窗中临近的图像帧位姿作为初值
        Vector3d P_inital = - R_inital * T[i];
        cv::eigen2cv(R_inital, tmp_r);
        cv::Rodrigues(tmp_r, rvec);
        cv::eigen2cv(P_inital, t);

        frame_it->second.is_key_frame = false;
        vector<cv::Point3f> pts_3_vector;
        vector<cv::Point2f> pts_2_vector;
        for (auto &id_pts : frame_it->second.points)  //对图像帧观测到的所有路标点进行遍历
        {
            int feature_id = id_pts.first;  //路标点的编号
            for (auto &i_p : id_pts.second)  //TODO(tzhang):对左右相机进行遍历（PS：该循环放在if判断后更好）
            {
                it = sfm_tracked_points.find(feature_id);
                if(it != sfm_tracked_points.end())
                {
                    Vector3d world_pts = it->second;  //得到路标点在世界坐标系中的位置
                    cv::Point3f pts_3(world_pts(0), world_pts(1), world_pts(2));
                    pts_3_vector.push_back(pts_3);
                    Vector2d img_pts = i_p.second.head<2>();  //得到路标点在归一化相机坐标系中的位置
                    cv::Point2f pts_2(img_pts(0), img_pts(1));
                    pts_2_vector.push_back(pts_2);
                }
            }
        }
        cv::Mat K = (cv::Mat_<double>(3, 3) << 1, 0, 0, 0, 1, 0, 0, 0, 1);     
        if(pts_3_vector.size() < 6)  //该图像帧不在滑窗中，且观测到的、已完成初始化的路标点数不足6个
        {
            cout << "pts_3_vector size " << pts_3_vector.size() << endl;
            ROS_DEBUG("Not enough points for solve pnp !");
            return false;
        }
        if (! cv::solvePnP(pts_3_vector, pts_2_vector, K, D, rvec, t, 1))
        {
            ROS_DEBUG("solve pnp fail!");
            return false;
        }
        cv::Rodrigues(rvec, r);
        MatrixXd R_pnp,tmp_R_pnp;
        cv::cv2eigen(r, tmp_R_pnp);
        R_pnp = tmp_R_pnp.transpose();  // 通过PnP求解得到R_w_c
        MatrixXd T_pnp;
        cv::cv2eigen(t, T_pnp);
        T_pnp = R_pnp * (-T_pnp);  // 通过PnP求解得到t_w_c
        frame_it->second.R = R_pnp * RIC[0].transpose();  //得到R_w_i
        frame_it->second.T = T_pnp;  //t_w_i （PS：未考虑camera与imu之间的平移,由于尺度因子未知，此时不用考虑；在求解出尺度因子后，会考虑）
    }
    if (visualInitialAlign())
        return true;
    else
    {
        ROS_INFO("misalign visual structure with IMU");
        return false;
    }

}

// 视觉和惯性的对其,对应https://mp.weixin.qq.com/s/9twYJMOE8oydAzqND0UmFw中的visualInitialAlign
/* visualInitialAlign
很具VIO课程第七讲:一共分为5步:
1估计旋转外参. 2估计陀螺仪bias 3估计中立方向,速度.尺度初始值 4对重力加速度进一步优化 5将轨迹对其到世界坐标系 */
bool Estimator::visualInitialAlign()
{
    TicToc t_g;
    VectorXd x;
    //solve scale
    bool result = VisualIMUAlignment(all_image_frame, Bgs, g, x);  //IMU与camera对准，计算更新了陀螺仪偏置bgs、重力向量g、尺度因子s、速度v
    if(!result)
    {
        ROS_DEBUG("solve g failed!");
        return false;
    }

    // change state
    for (int i = 0; i <= frame_count; i++)
    {
        Matrix3d Ri = all_image_frame[Headers[i]].R;  //Headers[i]存储图像帧时间戳，初始化过程中，仅边缘化老的图像帧，因此留在滑窗中的均为关键帧
        Vector3d Pi = all_image_frame[Headers[i]].T;
        Ps[i] = Pi;  //t_w_b
        Rs[i] = Ri;  //R_w_b
        all_image_frame[Headers[i]].is_key_frame = true;
    }

    double s = (x.tail<1>())(0);  //尺度因子
    for (int i = 0; i <= WINDOW_SIZE; i++)
    {
        pre_integrations[i]->repropagate(Vector3d::Zero(), Bgs[i]);  //得到陀螺仪bias新值，预积分重新传播；此处的预积分为Estimator类中的预积分；
                                                                    //图像帧中预积分的重新传播，在计算bg后已经完成
                                                                    //TODO(tzhang)：Estimator与图像帧中均维护预积分，重复，是否有必要？待优化
    }
    for (int i = frame_count; i >= 0; i--)  //利用尺度因子，对t_w_b的更新；注意此时用到了camera与imu之间的平移量
        Ps[i] = s * Ps[i] - Rs[i] * TIC[0] - (s * Ps[0] - Rs[0] * TIC[0]);
    int kv = -1;
    map<double, ImageFrame>::iterator frame_i;
    for (frame_i = all_image_frame.begin(); frame_i != all_image_frame.end(); frame_i++)  //更新Estimator中的速度，注意此时的速度值相对于世界坐标系；
                                                                                          //而初始化过程中的速度，相对于对应的机体坐标系
    {
        if(frame_i->second.is_key_frame)
        {
            kv++;
            Vs[kv] = frame_i->second.R * x.segment<3>(kv * 3);  //速度维度为3，故乘以3
        }
    }

    Matrix3d R0 = Utility::g2R(g);  //根据基于当前世界坐标系计算得到的重力方向与实际重力方向差异，计算当前世界坐标系的修正量；
                                    //注意：由于yaw不可观，修正量中剔除了yaw影响，也即仅将世界坐标系的z向与重力方向对齐
    double yaw = Utility::R2ypr(R0 * Rs[0]).x();
    R0 = Utility::ypr2R(Eigen::Vector3d{-yaw, 0, 0}) * R0;
    g = R0 * g;
    //Matrix3d rot_diff = R0 * Rs[0].transpose();
    Matrix3d rot_diff = R0;  //将世界坐标系与重力方向对齐，之前的世界坐标系Rs[0]根据图像帧定义得到，并未对准到重力方向
    for (int i = 0; i <= frame_count; i++)
    {
        Ps[i] = rot_diff * Ps[i];
        Rs[i] = rot_diff * Rs[i];
        Vs[i] = rot_diff * Vs[i];
    }
    ROS_DEBUG_STREAM("g0     " << g.transpose());
    ROS_DEBUG_STREAM("my R0  " << Utility::R2ypr(Rs[0]).transpose()); 

    f_manager.clearDepth();  //清除路标点状态，假定所有路标点逆深度均为估计；注意后续优化中路标点采用逆深度，而初始化过程中路标点采用三维坐标
    f_manager.triangulate(frame_count, Ps, Rs, tic, ric);  //基于SVD的路标点三角化，双目情形：利用左右图像信息； 非双目情形：利用前后帧图像信息

    return true;
}

bool Estimator::relativePose(Matrix3d &relative_R, Vector3d &relative_T, int &l)  //返回值：WINDOW_SIZE变换到l的旋转、平移；及图像帧index
{
    // find previous frame which contians enough correspondance and parallex with newest frame
    for (int i = 0; i < WINDOW_SIZE; i++)  //遍历滑窗中的图像帧（除去最后一个图像帧WINDOW_SIZE）
    {
        vector<pair<Vector3d, Vector3d>> corres;
        corres = f_manager.getCorresponding(i, WINDOW_SIZE);  //获取图像帧i与最后一个图像帧（WINDOW_SIZE）共视路标点在各自左归一化相机坐标系的坐标
        if (corres.size() > 20)  //图像帧i与最后一个图像帧之间的共视点数目大于20个才进行后续处理
        {
            double sum_parallax = 0;
            double average_parallax;
            for (int j = 0; j < int(corres.size()); j++)
            {
                Vector2d pts_0(corres[j].first(0), corres[j].first(1));
                Vector2d pts_1(corres[j].second(0), corres[j].second(1));
                double parallax = (pts_0 - pts_1).norm();
                sum_parallax = sum_parallax + parallax;  //计算归一化相机坐标系下的视差和

            }
            average_parallax = 1.0 * sum_parallax / int(corres.size());  //归一化相机坐标系下的平均视差
            if(average_parallax * 460 > 30 && m_estimator.solveRelativeRT(corres, relative_R, relative_T))
            {  //上述的460表示焦距f（尽管并不太严谨，具体相机焦距并不一定是460），从而在图像坐标系下评估视差
                l = i;
                ROS_DEBUG("average_parallax %f choose l %d and newest frame to triangulate the whole structure", average_parallax * 460, l);
                return true;
            }
        }
    }
    return false;
}

void Estimator::vector2double()
{
    for (int i = 0; i <= WINDOW_SIZE; i++)
    {
        para_Pose[i][0] = Ps[i].x();
        para_Pose[i][1] = Ps[i].y();
        para_Pose[i][2] = Ps[i].z();
        Quaterniond q{Rs[i]};
        para_Pose[i][3] = q.x();
        para_Pose[i][4] = q.y();
        para_Pose[i][5] = q.z();
        para_Pose[i][6] = q.w();

        if(USE_IMU)
        {
            para_SpeedBias[i][0] = Vs[i].x();
            para_SpeedBias[i][1] = Vs[i].y();
            para_SpeedBias[i][2] = Vs[i].z();

            para_SpeedBias[i][3] = Bas[i].x();
            para_SpeedBias[i][4] = Bas[i].y();
            para_SpeedBias[i][5] = Bas[i].z();

            para_SpeedBias[i][6] = Bgs[i].x();
            para_SpeedBias[i][7] = Bgs[i].y();
            para_SpeedBias[i][8] = Bgs[i].z();
        }
    }

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        para_Ex_Pose[i][0] = tic[i].x();
        para_Ex_Pose[i][1] = tic[i].y();
        para_Ex_Pose[i][2] = tic[i].z();
        Quaterniond q{ric[i]};
        para_Ex_Pose[i][3] = q.x();
        para_Ex_Pose[i][4] = q.y();
        para_Ex_Pose[i][5] = q.z();
        para_Ex_Pose[i][6] = q.w();
    }


    VectorXd dep = f_manager.getDepthVector();
    for (int i = 0; i < f_manager.getFeatureCount(); i++)
        para_Feature[i][0] = dep(i);

    para_Td[0][0] = td;
}

void Estimator::double2vector()
{
    Vector3d origin_R0 = Utility::R2ypr(Rs[0]);
    Vector3d origin_P0 = Ps[0];

    if (failure_occur)
    {
        origin_R0 = Utility::R2ypr(last_R0);
        origin_P0 = last_P0;
        failure_occur = 0;
    }

    if(USE_IMU)
    {
        Vector3d origin_R00 = Utility::R2ypr(Quaterniond(para_Pose[0][6],
                                                          para_Pose[0][3],
                                                          para_Pose[0][4],
                                                          para_Pose[0][5]).toRotationMatrix());
        double y_diff = origin_R0.x() - origin_R00.x();
        //TODO
        Matrix3d rot_diff = Utility::ypr2R(Vector3d(y_diff, 0, 0));
        if (abs(abs(origin_R0.y()) - 90) < 1.0 || abs(abs(origin_R00.y()) - 90) < 1.0)
        {
            ROS_DEBUG("euler singular point!");
            rot_diff = Rs[0] * Quaterniond(para_Pose[0][6],
                                           para_Pose[0][3],
                                           para_Pose[0][4],
                                           para_Pose[0][5]).toRotationMatrix().transpose();
        }

        for (int i = 0; i <= WINDOW_SIZE; i++)
        {

            Rs[i] = rot_diff * Quaterniond(para_Pose[i][6], para_Pose[i][3], para_Pose[i][4], para_Pose[i][5]).normalized().toRotationMatrix();
            
            Ps[i] = rot_diff * Vector3d(para_Pose[i][0] - para_Pose[0][0],
                                    para_Pose[i][1] - para_Pose[0][1],
                                    para_Pose[i][2] - para_Pose[0][2]) + origin_P0;


                Vs[i] = rot_diff * Vector3d(para_SpeedBias[i][0],
                                            para_SpeedBias[i][1],
                                            para_SpeedBias[i][2]);

                Bas[i] = Vector3d(para_SpeedBias[i][3],
                                  para_SpeedBias[i][4],
                                  para_SpeedBias[i][5]);

                Bgs[i] = Vector3d(para_SpeedBias[i][6],
                                  para_SpeedBias[i][7],
                                  para_SpeedBias[i][8]);
            
        }
    }
    else
    {
        for (int i = 0; i <= WINDOW_SIZE; i++)
        {
            Rs[i] = Quaterniond(para_Pose[i][6], para_Pose[i][3], para_Pose[i][4], para_Pose[i][5]).normalized().toRotationMatrix();
            
            Ps[i] = Vector3d(para_Pose[i][0], para_Pose[i][1], para_Pose[i][2]);
        }
    }

    if(USE_IMU)
    {
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            tic[i] = Vector3d(para_Ex_Pose[i][0],
                              para_Ex_Pose[i][1],
                              para_Ex_Pose[i][2]);
            ric[i] = Quaterniond(para_Ex_Pose[i][6],
                                 para_Ex_Pose[i][3],
                                 para_Ex_Pose[i][4],
                                 para_Ex_Pose[i][5]).normalized().toRotationMatrix();
        }
    }

    VectorXd dep = f_manager.getDepthVector();
    for (int i = 0; i < f_manager.getFeatureCount(); i++)
        dep(i) = para_Feature[i][0];
    f_manager.setDepth(dep);

    if(USE_IMU)
        td = para_Td[0][0];

}

bool Estimator::failureDetection()
{
    return false;  //TODO(tzhang):失败检测策略还可自己探索
    if (f_manager.last_track_num < 2)
    {
        ROS_INFO(" little feature %d", f_manager.last_track_num);
        //return true;
    }
    if (Bas[WINDOW_SIZE].norm() > 2.5)
    {
        ROS_INFO(" big IMU acc bias estimation %f", Bas[WINDOW_SIZE].norm());
        return true;
    }
    if (Bgs[WINDOW_SIZE].norm() > 1.0)
    {
        ROS_INFO(" big IMU gyr bias estimation %f", Bgs[WINDOW_SIZE].norm());
        return true;
    }
    /*
    if (tic(0) > 1)
    {
        ROS_INFO(" big extri param estimation %d", tic(0) > 1);
        return true;
    }
    */
    Vector3d tmp_P = Ps[WINDOW_SIZE];
    if ((tmp_P - last_P).norm() > 5)
    {
        //ROS_INFO(" big translation");
        //return true;
    }
    if (abs(tmp_P.z() - last_P.z()) > 1)
    {
        //ROS_INFO(" big z translation");
        //return true; 
    }
    Matrix3d tmp_R = Rs[WINDOW_SIZE];
    Matrix3d delta_R = tmp_R.transpose() * last_R;
    Quaterniond delta_Q(delta_R);
    double delta_angle;
    delta_angle = acos(delta_Q.w()) * 2.0 / 3.14 * 180.0;
    if (delta_angle > 50)
    {
        ROS_INFO(" big delta_angle ");
        //return true;
    }
    return false;
}

/**
 * google中ceres的问题，待估计的参数包括滑窗内所有帧的位姿，速度，加速度的漂移，陀螺仪的漂移（前面三项体现在para_speedBias里面，只一个9自由度的向量），
 * 以及特征点的深度（这一项是让整个非线性优化维度变得很高的主要原因，但是矩阵稀疏，有方便的求解方法）。
 * 如果相机到IMU的外参在配置文件没有足够的勇气设置成0,这两项R,T也会作为参数进行估计，
 * 另外如果使用比较low的自制VIsensor没有做到相机IMU同步，
 * 这两个传感器的时间差td也会作为一个参数来进行非线性优化（配置文件estimator_td如果你没有勇气设置成0的话，这一项也会进行估计）。
 * 单目的残差项与VINS-mono一致，但是双目残差项多了两项。还未看懂，谢谢惠顾
 * 
 * 基于滑动窗口的紧耦合的非线性优化，残差项的构造和求解
 * */
void Estimator::optimization()
{
    TicToc t_whole, t_prepare;
    vector2double();  //状态向量用ceres优化方便使用的形式存储

    //------------------ 定义问题 定义本地参数化,并添加优化参数-------------------------------------------------
    ceres::Problem problem; // 定义ceres的优化问题
    ceres::LossFunction *loss_function; //核函数
    //loss_function = NULL;
    loss_function = new ceres::HuberLoss(1.0);  //HuberLoss当预测偏差小于 δ 时，它采用平方误差,当预测偏差大于 δ 时，采用的线性误差。
    //loss_function = new ceres::CauchyLoss(1.0 / FOCAL_LENGTH);
    //ceres::LossFunction* loss_function = new ceres::HuberLoss(1.0);

    /*######优化参数：q、p；v、Ba、Bg#######*/
    for (int i = 0; i < frame_count + 1; i++)  //IMU存在时，frame_count等于WINDOW_SIZE才会调用optimization() tzhang
    {
        // 对于四元数或者旋转矩阵这种使用过参数化表示旋转的方式，它们是不支持广义的加法
        // 所以我们在使用ceres对其进行迭代更新的时候就需要自定义其更新方式了，具体的做法是实现一个LocalParameterization
        ceres::LocalParameterization *local_parameterization = new PoseLocalParameterization();
        
        // AddParameterBlock   向该问题添加具有适当大小和参数化的参数块。
        problem.AddParameterBlock(para_Pose[i], SIZE_POSE, local_parameterization);  //q、p参数
        if(USE_IMU)
            problem.AddParameterBlock(para_SpeedBias[i], SIZE_SPEEDBIAS);  //v、Ba、Bg参数
    }
    
    // 没使用imu时,将窗口内第一帧的位姿固定
    if(!USE_IMU)
        // SetParameterBlockConstant 在优化过程中，使指示的参数块保持恒定。设置任何参数块变成一个常量
        // 固定第一帧的位姿不变!  这里涉及到论文2中的
        problem.SetParameterBlockConstant(para_Pose[0]);  //双目版本时，Rs[0]、Ps[0]固定

    /*######优化参数：imu与camera外参#######*/
    for (int i = 0; i < NUM_OF_CAM; i++)  //imu与camera外参
    {
        ceres::LocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(para_Ex_Pose[i], SIZE_POSE, local_parameterization);
        if ((ESTIMATE_EXTRINSIC && frame_count == WINDOW_SIZE && Vs[0].norm() > 0.2) || openExEstimation)
        {
            //ROS_INFO("estimate extinsic param");
            openExEstimation = 1; //打开外部估计
        }
        else //如果不需要估计,则把估计器中的外部参数设为定值
        {
            //ROS_INFO("fix extinsic param");
            problem.SetParameterBlockConstant(para_Ex_Pose[i]);
        }
    }

    /*######优化参数：imu与camera之间的time offset#######*/
    problem.AddParameterBlock(para_Td[0], 1); //把时间也作为待优化变量
    if (!ESTIMATE_TD || Vs[0].norm() < 0.2)  //速度过低时，不估计td；//如果不估计时间就固定
        problem.SetParameterBlockConstant(para_Td[0]);
    
    // ------------------------在问题中添加约束,也就是构造残差函数---------------------------------- 
    // 在问题中添加先验信息作为约束
    //构建残差
    /*******先验残差*******/
    if (last_marginalization_info && last_marginalization_info->valid)  
    {
        // construct new marginlization_factor  会调用marginalization_factor——>Evaluate计算边缘化后的残差与雅克比
        // 构造新的marginisation_factor construct new marginlization_factor
        MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
        
        /* 通过提供参数块的向量来添加残差块。
        ResidualBlockId AddResidualBlock(
            CostFunction* cost_function,//损失函数
            LossFunction* loss_function,//核函数
            const std::vector<double*>& parameter_blocks); */
        problem.AddResidualBlock(marginalization_factor, NULL,
                                 last_marginalization_parameter_blocks);
    }
    // 在问题中添加IMU约束
    /*******预积分残差*******/
    if(USE_IMU)
    {
        for (int i = 0; i < frame_count; i++)  //预积分残差，总数目为frame_count
        {
            int j = i + 1;
            if (pre_integrations[j]->sum_dt > 10.0)  //两图像帧之间时间过长，不使用中间的预积分 tzhang
                continue;
            IMUFactor* imu_factor = new IMUFactor(pre_integrations[j]);
            //添加残差格式：残差因子，鲁棒核函数，优化变量（i时刻位姿，i时刻速度与偏置，i+1时刻位姿，i+1时刻速度与偏置）
            problem.AddResidualBlock(imu_factor, NULL, para_Pose[i], para_SpeedBias[i], para_Pose[j], para_SpeedBias[j]);
            //这里添加的参数包括状态i和状态j
        }
    }

    /*******重投影残差*******/
    //重投影残差相关，此时使用了Huber损失核函数
    int f_m_cnt = 0;  //每个特征点,观测到它的相机的计数 visual measurement count
    int feature_index = -1;
    for (auto &it_per_id : f_manager.feature)  //遍历路标点
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();  //路标点被观测的次数
        if (it_per_id.used_num < 4)
            continue;
 
        ++feature_index;
        
        // imu_i该特征点第一次被观测到的帧 ,imu_j = imu_i - 1
        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
        
        Vector3d pts_i = it_per_id.feature_per_frame[0].point;  //用于计算估计值

        for (auto &it_per_frame : it_per_id.feature_per_frame)  //遍历观测到路标点的图像帧
        {
            imu_j++;
            if (imu_i != imu_j) //既,本次不是第一次观测到
            {
                Vector3d pts_j = it_per_frame.point;  //测量值
                //左相机在i时刻和j时刻分别观测到路标点
                ProjectionTwoFrameOneCamFactor *f_td = new ProjectionTwoFrameOneCamFactor(pts_i, pts_j, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocity,
                                                                 it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                problem.AddResidualBlock(f_td, loss_function, para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index], para_Td[0]);
                
                /* 相关介绍:
                    1 只在视觉量测中用了核函数loss_function 用的是huber
                    2 参数包含了para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index], para_Td[0]
                    3 ProjectionTwoFrameOneCamFactor这个重投影并不是很懂 */
            }
            
            // 如果是双目的
            if(STEREO && it_per_frame.is_stereo)
            {                
                Vector3d pts_j_right = it_per_frame.pointRight;
                if(imu_i != imu_j)  //既,本次不是第一次观测到
                {   //左相机在i时刻、右相机在j时刻分别观测到路标点
                    ProjectionTwoFrameTwoCamFactor *f = new ProjectionTwoFrameTwoCamFactor(pts_i, pts_j_right, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocityRight,
                                                                 it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                    problem.AddResidualBlock(f, loss_function, para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Ex_Pose[1], para_Feature[feature_index], para_Td[0]);
                }
                else //既,本次是第一次观测到
                {   //左相机和右相机在i时刻分别观测到路标点
                    ProjectionOneFrameTwoCamFactor *f = new ProjectionOneFrameTwoCamFactor(pts_i, pts_j_right, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocityRight,
                                                                 it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                    problem.AddResidualBlock(f, loss_function, para_Ex_Pose[0], para_Ex_Pose[1], para_Feature[feature_index], para_Td[0]);
                }
               
            }
            f_m_cnt++;
        }
    }

    ROS_DEBUG("visual measurement count: %d", f_m_cnt);
    //printf("prepare for ceres: %f \n", t_prepare.toc());

     // ------------------------------------写下来配置优化选项,并进行求解-----------------------------------------
    //优化参数配置
    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;  //normal equation求解方法
    //options.num_threads = 2;
    options.trust_region_strategy_type = ceres::DOGLEG;  //非线性优化求解方法，狗腿法
    options.max_num_iterations = NUM_ITERATIONS;  //最大迭代次数
    //options.use_explicit_schur_complement = true;
    //options.minimizer_progress_to_stdout = true;
    //options.use_nonmonotonic_steps = true;
    if (marginalization_flag == MARGIN_OLD)  //最大求解时间
        options.max_solver_time_in_seconds = SOLVER_TIME * 4.0 / 5.0;
    else
        options.max_solver_time_in_seconds = SOLVER_TIME;
    TicToc t_solver;
    ceres::Solver::Summary summary; //优化信息
    ceres::Solve(options, &problem, &summary); //非线性优化求解
    //cout << summary.BriefReport() << endl;
    ROS_DEBUG("Iterations : %d", static_cast<int>(summary.iterations.size()));
    //printf("solver costs: %f \n", t_solver.toc());

    double2vector();  //优化求解完成，ceres优化方便使用的数组形式转存至状态向量（把ceres求解出来的结果附加到滑窗内的变量中去）
    //printf("frame_count: %d \n", frame_count);

    if(frame_count < WINDOW_SIZE)  //滑窗未满
        return;

    // -----------------------------marginalization------------------------------------
    /*%%%%%滑窗满了，进行边缘化处理%%%%%%%*/
    TicToc t_whole_marginalization;  
    //如果需要marg掉最老的一帧
    if (marginalization_flag == MARGIN_OLD)   //将最老的图像帧数据边缘化； tzhang
    {
        MarginalizationInfo *marginalization_info = new MarginalizationInfo();//先验信息
        vector2double();  //状态向量转存为数组形式

        // 先验部分，基于先验残差，边缘化滑窗中第0帧时刻的状态向量
        if (last_marginalization_info && last_marginalization_info->valid)
        {
            vector<int> drop_set; //边缘话的优化变量的位置_drop_set
            for (int i = 0; i < static_cast<int>(last_marginalization_parameter_blocks.size()); i++)
                //last_marginalization_parameter_blocks 是上一轮留下来的残差块
            {
                if (last_marginalization_parameter_blocks[i] == para_Pose[0] ||  //丢弃滑窗中第0帧时刻的位姿、速度、偏置
                    last_marginalization_parameter_blocks[i] == para_SpeedBias[0])
                    //需要marg掉的优化变量，也就是滑窗内第一个变量,para_Pose[0]和para_SpeedBias[0]
                    drop_set.push_back(i);
            }
            // construct new marginlization_factor 创建新的marg因子
            MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
            
            // 是为了将不同的损失函数_cost_function以及优化变量_parameter_blocks统一起来再一起添加到marginalization_info中
            // ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
            //                                                                vector<double *>{para_Pose[0], para_SpeedBias[0]},
            //                                                                vector<int>{0, 1});  
            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
                                                                           last_marginalization_parameter_blocks,
                                                                           drop_set);  
            // 将上一步marginalization后的信息作为先验信息
            marginalization_info->addResidualBlockInfo(residual_block_info);
        }
        
        // 添加IMU的marg信息
        // 然后添加第0帧和第1帧之间的IMU预积分值以及第0帧和第1帧相关优化变量
        //imu 预积分部分，基于第0帧与第1帧之间的预积分残差，边缘化第0帧状态向量
        if(USE_IMU)
        {
            if (pre_integrations[1]->sum_dt < 10.0)
            {
                IMUFactor* imu_factor = new IMUFactor(pre_integrations[1]);
                // 这一步添加IMU的marg信息
                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(imu_factor, NULL,
                                                                           vector<double *>{para_Pose[0], para_SpeedBias[0], para_Pose[1], para_SpeedBias[1]},//优化变量
                                                                           vector<int>{0, 1});  //这里是0,1的原因是0和1是para_Pose[0], para_SpeedBias[0]是需要marg的变量
                marginalization_info->addResidualBlockInfo(residual_block_info);
            }
        }

        //图像部分，基于与第0帧相关的图像残差，边缘化第一次观测的图像帧为第0帧的路标点和第0帧
        // 添加视觉的maeg信息
        {
            int feature_index = -1;
            
            //这里是遍历滑窗所有的特征点
            for (auto &it_per_id : f_manager.feature)  //对路标点的遍历
            {
                it_per_id.used_num = it_per_id.feature_per_frame.size();
                if (it_per_id.used_num < 4)
                    continue;

                ++feature_index;

                int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;//这里是从特征点的第一个观察帧开始
                if (imu_i != 0)  //仅处理第一次观测的图像帧为第0帧的情形
                                 //如果第一个观察帧不是第一帧就不进行考虑，因此后面用来构建marg矩阵的都是和第一帧有共视关系的滑窗帧
                    continue;

                Vector3d pts_i = it_per_id.feature_per_frame[0].point;

                for (auto &it_per_frame : it_per_id.feature_per_frame)  //对观测到路标点的图像帧的遍历
                {
                    imu_j++;
                    if(imu_i != imu_j)
                    {
                        Vector3d pts_j = it_per_frame.point;
                        //左相机在i时刻、在j时刻分别观测到路标点
                        ProjectionTwoFrameOneCamFactor *f_td = new ProjectionTwoFrameOneCamFactor(pts_i, pts_j, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocity,
                                                                          it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                        ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f_td, loss_function,
                                                                                        vector<double *>{para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Feature[feature_index], para_Td[0]},//优化变量
                                                                                        vector<int>{0, 3});  //边缘化para_Pose[0]与para_Feature[feature_index]
                        marginalization_info->addResidualBlockInfo(residual_block_info);
                    }
                    if(STEREO && it_per_frame.is_stereo)
                    {
                        Vector3d pts_j_right = it_per_frame.pointRight;
                        if(imu_i != imu_j)
                        {
                            //左相机在i时刻、右相机在j时刻分别观测到路标点
                            ProjectionTwoFrameTwoCamFactor *f = new ProjectionTwoFrameTwoCamFactor(pts_i, pts_j_right, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocityRight,
                                                                          it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                                                                           vector<double *>{para_Pose[imu_i], para_Pose[imu_j], para_Ex_Pose[0], para_Ex_Pose[1], para_Feature[feature_index], para_Td[0]},//优化变量
                                                                                           vector<int>{0, 4});  //边缘化para_Pose[0]与para_Feature[feature_index] 
                                                                                                         //为0和3的原因是，para_Pose[imu_i]是第一帧的位姿，需要marg掉，而3是para_Feature[feature_index]是和第一帧相关的特征点，需要marg掉 
                            marginalization_info->addResidualBlockInfo(residual_block_info);
                        }
                        else
                        {
                            //左相机在i时刻、右相机在i时刻分别观测到路标点
                            ProjectionOneFrameTwoCamFactor *f = new ProjectionOneFrameTwoCamFactor(pts_i, pts_j_right, it_per_id.feature_per_frame[0].velocity, it_per_frame.velocityRight,
                                                                          it_per_id.feature_per_frame[0].cur_td, it_per_frame.cur_td);
                            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                                                                           vector<double *>{para_Ex_Pose[0], para_Ex_Pose[1], para_Feature[feature_index], para_Td[0]},
                                                                                           vector<int>{2});  //边缘化para_Feature[feature_index]
                            marginalization_info->addResidualBlockInfo(residual_block_info);
                        }
                    }
                }
            }
        }
        
        TicToc t_pre_margin;
        
        // 上面通过调用 addResidualBlockInfo() 已经确定优化变量的数量、存储位置、长度以及待优化变量的数量以及存储位置，
        //-------------------------- 下面就需要调用 preMarginalize() 进行预处理
        marginalization_info->preMarginalize();
        ROS_DEBUG("pre marginalization %f ms", t_pre_margin.toc());
        
        //------------------------调用 marginalize 正式开始边缘化
        TicToc t_margin;
        marginalization_info->marginalize();
        ROS_DEBUG("marginalization %f ms", t_margin.toc());
        
        //------------------------在optimization的最后会有一部滑窗预移动的操作
        // 值得注意的是，这里仅仅是相当于将指针进行了一次移动，指针对应的数据还是旧数据，因此需要结合后面调用的 slideWindow() 函数才能实现真正的滑窗移动
        
        //仅仅改变滑窗double部分地址映射，具体值的通过slideWindow和vector2double函数完成；记住边缘化仅仅改变A和b，不改变状态向量
        //由于第0帧观测到的路标点全被边缘化，即边缘化后保存的状态向量中没有路标点;因此addr_shift无需添加路标点
        std::unordered_map<long, double *> addr_shift;
        for (int i = 1; i <= WINDOW_SIZE; i++)  //最老图像帧数据丢弃，从i=1开始遍历
        {
            //这一步的操作指的是第i的位置存放的的是i-1的内容，这就意味着窗口向前移动了一格
            addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i - 1];  // i数据保存到1-1指向的地址，滑窗向前移动一格 //因此para_Pose这些变量都是双指针变量，因此这一步是指针操作
            if(USE_IMU)
                addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i - 1];
        }
        for (int i = 0; i < NUM_OF_CAM; i++)
            addr_shift[reinterpret_cast<long>(para_Ex_Pose[i])] = para_Ex_Pose[i];

        addr_shift[reinterpret_cast<long>(para_Td[0])] = para_Td[0];

        vector<double *> parameter_blocks = marginalization_info->getParameterBlocks(addr_shift);

        if (last_marginalization_info)
            delete last_marginalization_info; //删除掉上一次的marg相关的内容
        last_marginalization_info = marginalization_info; //marg相关内容的递归
        last_marginalization_parameter_blocks = parameter_blocks; //优化变量的递归，这里面仅仅是指针
    }
    else   //将次新的图像帧数据边缘化； tzhang
    {
        if (last_marginalization_info && //存在先验边缘化信息时才进行次新帧边缘化;否则仅仅通过slidewindow，丢弃次新帧
            std::count(std::begin(last_marginalization_parameter_blocks), std::end(last_marginalization_parameter_blocks), para_Pose[WINDOW_SIZE - 1]))
        {

            MarginalizationInfo *marginalization_info = new MarginalizationInfo();
            vector2double();
            if (last_marginalization_info && last_marginalization_info->valid)
            {
                vector<int> drop_set;  //记录需要丢弃的变量在last_marginalization_parameter_blocks中的索引
                for (int i = 0; i < static_cast<int>(last_marginalization_parameter_blocks.size()); i++)
                {
                    ROS_ASSERT(last_marginalization_parameter_blocks[i] != para_SpeedBias[WINDOW_SIZE - 1]);
                    if (last_marginalization_parameter_blocks[i] == para_Pose[WINDOW_SIZE - 1])  //TODO(tzhang):仅仅只边缘化WINDOW_SIZE - 1位姿变量， 对其特征点、图像数据不进行处理
                        drop_set.push_back(i);
                }
                // construct new marginlization_factor
                MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info);
                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
                                                                               last_marginalization_parameter_blocks,
                                                                               drop_set);

                marginalization_info->addResidualBlockInfo(residual_block_info);
            }

            TicToc t_pre_margin;
            ROS_DEBUG("begin marginalization");
            marginalization_info->preMarginalize();  //构建parameter_block_data
            ROS_DEBUG("end pre marginalization, %f ms", t_pre_margin.toc());

            TicToc t_margin;
            ROS_DEBUG("begin marginalization");
            marginalization_info->marginalize();
            ROS_DEBUG("end marginalization, %f ms", t_margin.toc());

            //仅仅改变滑窗double部分地址映射，具体值的更改在slideWindow和vector2double函数完成
            //由于边缘化次新帧，边缘化的状态向量仅为para_Pose[WINDOW_SIZE - 1];而保留的状态向量为在上一次边缘化得到的保留部分基础上、剔除para_Pose[WINDOW_SIZE - 1]的结果;
            //因此，边缘化次新帧得到的保留部分也未包含路标点，因此addr_shift无需添加路标点
            std::unordered_map<long, double *> addr_shift;
            for (int i = 0; i <= WINDOW_SIZE; i++)
            {
                if (i == WINDOW_SIZE - 1)  //WINDOW_SIZE - 1会被边缘化，不保存
                    continue;
                else if (i == WINDOW_SIZE)  //WINDOW_SIZE数据保存到WINDOW_SIZE-1指向的地址
                {
                    addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i - 1];
                    if(USE_IMU)
                        addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i - 1];
                }
                else  //其余的保存地址不变
                {
                    addr_shift[reinterpret_cast<long>(para_Pose[i])] = para_Pose[i];
                    if(USE_IMU)
                        addr_shift[reinterpret_cast<long>(para_SpeedBias[i])] = para_SpeedBias[i];
                }
            }
            for (int i = 0; i < NUM_OF_CAM; i++)
                addr_shift[reinterpret_cast<long>(para_Ex_Pose[i])] = para_Ex_Pose[i];

            addr_shift[reinterpret_cast<long>(para_Td[0])] = para_Td[0];

            
            vector<double *> parameter_blocks = marginalization_info->getParameterBlocks(addr_shift);  //提取保存的数据
            if (last_marginalization_info)
                delete last_marginalization_info;
            last_marginalization_info = marginalization_info;
            last_marginalization_parameter_blocks = parameter_blocks;
            
        }
    }
    //printf("whole marginalization costs: %f \n", t_whole_marginalization.toc());
    //printf("whole time for ceres: %f \n", t_whole.toc());
}

// 滑动窗口法,就是把前后元素交换
void Estimator::slideWindow()
{
    TicToc t_margin;
    if (marginalization_flag == MARGIN_OLD)  // 边缘化最老的图像帧，即次新的图像帧为关键帧
    {
        double t_0 = Headers[0];
        back_R0 = Rs[0];
        back_P0 = Ps[0];
        if (frame_count == WINDOW_SIZE)  //仅在滑窗满时，进行滑窗边缘化处理
        {
            //1、滑窗中的数据往前移动一帧；运行结果就是WINDOW_SIZE位置的状态为之前0位置对应的状态
            // 0,1,2...WINDOW_SIZE——>1,2...WINDOW_SIZE,0
            for (int i = 0; i < WINDOW_SIZE; i++)  
            {
                Headers[i] = Headers[i + 1];
                Rs[i].swap(Rs[i + 1]);
                Ps[i].swap(Ps[i + 1]);
                if(USE_IMU)
                {
                    std::swap(pre_integrations[i], pre_integrations[i + 1]);

                    dt_buf[i].swap(dt_buf[i + 1]);
                    linear_acceleration_buf[i].swap(linear_acceleration_buf[i + 1]);
                    angular_velocity_buf[i].swap(angular_velocity_buf[i + 1]);

                    Vs[i].swap(Vs[i + 1]);
                    Bas[i].swap(Bas[i + 1]);
                    Bgs[i].swap(Bgs[i + 1]);
                }
            }
            //2、处理前，WINDOW_SIZE位置的状态为之前0位置对应的状态；处理后，WINDOW_SIZE位置的状态为之前WINDOW_SIZE位置对应的状态;之前0位置对应的状态被剔除
            // 0,1,2...WINDOW_SIZE——>1,2...WINDOW_SIZE,WINDOW_SIZE
            Headers[WINDOW_SIZE] = Headers[WINDOW_SIZE - 1];
            Ps[WINDOW_SIZE] = Ps[WINDOW_SIZE - 1];
            Rs[WINDOW_SIZE] = Rs[WINDOW_SIZE - 1];

            if(USE_IMU)
            {
                Vs[WINDOW_SIZE] = Vs[WINDOW_SIZE - 1];
                Bas[WINDOW_SIZE] = Bas[WINDOW_SIZE - 1];
                Bgs[WINDOW_SIZE] = Bgs[WINDOW_SIZE - 1];

                delete pre_integrations[WINDOW_SIZE];
                pre_integrations[WINDOW_SIZE] = new IntegrationBase{acc_0, gyr_0, Bas[WINDOW_SIZE], Bgs[WINDOW_SIZE]};

                dt_buf[WINDOW_SIZE].clear();
                linear_acceleration_buf[WINDOW_SIZE].clear();
                angular_velocity_buf[WINDOW_SIZE].clear();
            }
            //3、对时刻t_0(对应滑窗第0帧)之前的所有数据进行剔除；即all_image_frame中仅保留滑窗中图像帧0与图像帧WINDOW_SIZE之间的数据
            if (true || solver_flag == INITIAL)
            {
                map<double, ImageFrame>::iterator it_0;
                it_0 = all_image_frame.find(t_0);
                delete it_0->second.pre_integration;
                all_image_frame.erase(all_image_frame.begin(), it_0);
            }
            slideWindowOld();
        }
    }
    else  //边缘化次新的图像帧，主要完成的工作是数据衔接 tzhang
    {
        if (frame_count == WINDOW_SIZE)  //仅在滑窗满时，进行滑窗边缘化处理
        {  //0,1,2...WINDOW_SIZE-2, WINDOW_SIZE-1, WINDOW_SIZE——>0,,1,2...WINDOW_SIZE-2,WINDOW_SIZE, WINDOW_SIZE
            Headers[frame_count - 1] = Headers[frame_count];
            Ps[frame_count - 1] = Ps[frame_count];
            Rs[frame_count - 1] = Rs[frame_count];

            if(USE_IMU)  //IMU数据衔接，预积分的传播
            {
                for (unsigned int i = 0; i < dt_buf[frame_count].size(); i++)
                {
                    double tmp_dt = dt_buf[frame_count][i];
                    Vector3d tmp_linear_acceleration = linear_acceleration_buf[frame_count][i];
                    Vector3d tmp_angular_velocity = angular_velocity_buf[frame_count][i];

                    pre_integrations[frame_count - 1]->push_back(tmp_dt, tmp_linear_acceleration, tmp_angular_velocity);  //预积分的传播

                    dt_buf[frame_count - 1].push_back(tmp_dt);  //TODO(tzhang): 数据保存有冗余，integration_base中也保存了同样的数据
                    linear_acceleration_buf[frame_count - 1].push_back(tmp_linear_acceleration);
                    angular_velocity_buf[frame_count - 1].push_back(tmp_angular_velocity);
                }

                Vs[frame_count - 1] = Vs[frame_count];
                Bas[frame_count - 1] = Bas[frame_count];
                Bgs[frame_count - 1] = Bgs[frame_count];

                delete pre_integrations[WINDOW_SIZE];
                pre_integrations[WINDOW_SIZE] = new IntegrationBase{acc_0, gyr_0, Bas[WINDOW_SIZE], Bgs[WINDOW_SIZE]};

                dt_buf[WINDOW_SIZE].clear();
                linear_acceleration_buf[WINDOW_SIZE].clear();
                angular_velocity_buf[WINDOW_SIZE].clear();
            }
            slideWindowNew();  //更新第一次观测到路标点的图像帧的索引
        }
    }
}

void Estimator::slideWindowNew()
{
    sum_of_front++;
    f_manager.removeFront(frame_count);
}

void Estimator::slideWindowOld()
{
    sum_of_back++;

    bool shift_depth = solver_flag == NON_LINEAR ? true : false;
    if (shift_depth)  //非线性优化阶段，除了更新第一次观测到路标点的图像帧的索引，还需更新路标点的逆深度
    {
        Matrix3d R0, R1;  //R0、P0表示被边缘化的图像帧，即老的第0帧的位姿； R1、P1表示新的第0帧的位姿 tzhang
        Vector3d P0, P1;
        R0 = back_R0 * ric[0];  //R_w_cl  老的第0帧左相机坐标系与世界坐标系之间的相对旋转
        R1 = Rs[0] * ric[0];    //R_w_cl  新的第0帧左相机坐标系与世界坐标系之间的相对旋转
        P0 = back_P0 + back_R0 * tic[0];
        P1 = Ps[0] + Rs[0] * tic[0];
        f_manager.removeBackShiftDepth(R0, P0, R1, P1);
    }
    else
        f_manager.removeBack();  //初始化未完成，只是更新第一次观测到路标点的图像帧的索引  tzhang
}


void Estimator::getPoseInWorldFrame(Eigen::Matrix4d &T)
{
    T = Eigen::Matrix4d::Identity();
    T.block<3, 3>(0, 0) = Rs[frame_count];
    T.block<3, 1>(0, 3) = Ps[frame_count];
}

void Estimator::getPoseInWorldFrame(int index, Eigen::Matrix4d &T)
{
    T = Eigen::Matrix4d::Identity();
    T.block<3, 3>(0, 0) = Rs[index];
    T.block<3, 1>(0, 3) = Ps[index];
}

void Estimator::predictPtsInNextFrame()
{
    //printf("predict pts in next frame\n");
    if(frame_count < 2)
        return;
    // predict next pose. Assume constant velocity motion
    Eigen::Matrix4d curT, prevT, nextT;  //T_w_c, from left camera to world frame，分别为当前时刻、前一时刻、下一时刻
    getPoseInWorldFrame(curT);  // 通过Rs、Ps获取位姿
    getPoseInWorldFrame(frame_count - 1, prevT);
    nextT = curT * (prevT.inverse() * curT);  //基于恒速模型，预测下一时刻位姿
    map<int, Eigen::Vector3d> predictPts;

    for (auto &it_per_id : f_manager.feature)
    {
        if(it_per_id.estimated_depth > 0)  //仅对已经初始化的路标点进行预测
        {
            int firstIndex = it_per_id.start_frame;  //第一次观测到该路标点的图像帧的index
            int lastIndex = it_per_id.start_frame + it_per_id.feature_per_frame.size() - 1;  //最后观测到该路标点的图像帧的index
            //printf("cur frame index  %d last frame index %d\n", frame_count, lastIndex);
            if((int)it_per_id.feature_per_frame.size() >= 2 && lastIndex == frame_count)  //仅对观测次数不小于两次、且在最新图像帧中观测到的路标点进行预测
            {
                double depth = it_per_id.estimated_depth;  //逆深度，在start_frame图像帧中表示
                Vector3d pts_j = ric[0] * (depth * it_per_id.feature_per_frame[0].point) + tic[0];  //路标点在start_frame图像帧时刻，相机坐标系下的坐标
                Vector3d pts_w = Rs[firstIndex] * pts_j + Ps[firstIndex];  //路标点在世界坐标系下的坐标
                Vector3d pts_local = nextT.block<3, 3>(0, 0).transpose() * (pts_w - nextT.block<3, 1>(0, 3));  //路标在在下一时刻（预测的）体坐标系下坐标
                Vector3d pts_cam = ric[0].transpose() * (pts_local - tic[0]);  ////路标在在下一时刻（预测的）相机坐标系下坐标
                int ptsIndex = it_per_id.feature_id;
                predictPts[ptsIndex] = pts_cam;  // 根据路标点编号，存储预测的坐标
            }
        }
    }
    featureTracker.setPrediction(predictPts);
    //printf("estimator output %d predict pts\n",(int)predictPts.size());
}

double Estimator::reprojectionError(Matrix3d &Ri, Vector3d &Pi, Matrix3d &rici, Vector3d &tici,
                                 Matrix3d &Rj, Vector3d &Pj, Matrix3d &ricj, Vector3d &ticj, 
                                 double depth, Vector3d &uvi, Vector3d &uvj)
{
    Vector3d pts_w = Ri * (rici * (depth * uvi) + tici) + Pi;  //路标点在世界坐标系下的坐标
    Vector3d pts_cj = ricj.transpose() * (Rj.transpose() * (pts_w - Pj) - ticj);  //路标点在j时刻左或右相机坐标系下的坐标
    Vector2d residual = (pts_cj / pts_cj.z()).head<2>() - uvj.head<2>();  //归一化相机坐标系下的重投影误差
    double rx = residual.x();
    double ry = residual.y();
    return sqrt(rx * rx + ry * ry);  //返回重投影误差均方根
}

void Estimator::outliersRejection(set<int> &removeIndex)
{
    //return;
    int feature_index = -1;
    for (auto &it_per_id : f_manager.feature)  // 遍历所有路标点
    {
        double err = 0;
        int errCnt = 0;
        it_per_id.used_num = it_per_id.feature_per_frame.size();  //也即观察到该路标点的图像帧数目 tzhang
        if (it_per_id.used_num < 4)  // 对观测少于4次的路标点，不进行外点判断
            continue;
        feature_index ++;
        int imu_i = it_per_id.start_frame, imu_j = imu_i - 1;
        Vector3d pts_i = it_per_id.feature_per_frame[0].point;
        double depth = it_per_id.estimated_depth;
        for (auto &it_per_frame : it_per_id.feature_per_frame)
        {
            imu_j++;
            if (imu_i != imu_j)  //不同时刻，左相机在不同帧之间的重投影误差计算 
            {
                Vector3d pts_j = it_per_frame.point;             
                double tmp_error = reprojectionError(Rs[imu_i], Ps[imu_i], ric[0], tic[0], 
                                                    Rs[imu_j], Ps[imu_j], ric[0], tic[0],
                                                    depth, pts_i, pts_j);
                err += tmp_error;
                errCnt++;
                //printf("tmp_error %f\n", FOCAL_LENGTH / 1.5 * tmp_error);
            }
            // need to rewrite projecton factor.........
            if(STEREO && it_per_frame.is_stereo)  // 双目情形
            {
                
                Vector3d pts_j_right = it_per_frame.pointRight;
                if(imu_i != imu_j)  //不同时刻，左右图像帧之间的重投影误差
                {            
                    double tmp_error = reprojectionError(Rs[imu_i], Ps[imu_i], ric[0], tic[0], 
                                                        Rs[imu_j], Ps[imu_j], ric[1], tic[1],
                                                        depth, pts_i, pts_j_right);
                    err += tmp_error;
                    errCnt++;
                    //printf("tmp_error %f\n", FOCAL_LENGTH / 1.5 * tmp_error);
                }
                else  //相同时刻，左右图像帧之间的重投影误差 TODO(tzhang)：此处不同时刻判断没啥用，代码冗余
                {
                    double tmp_error = reprojectionError(Rs[imu_i], Ps[imu_i], ric[0], tic[0], //
                                                        Rs[imu_j], Ps[imu_j], ric[1], tic[1],
                                                        depth, pts_i, pts_j_right);
                    err += tmp_error;
                    errCnt++;
                    //printf("tmp_error %f\n", FOCAL_LENGTH / 1.5 * tmp_error);
                }       
            }
        }
        double ave_err = err / errCnt;
        if(ave_err * FOCAL_LENGTH > 3)  // 若平均的重投影均方根过大，则判定该路标点为外点; 添加该路标点编号至removeIndex中
            removeIndex.insert(it_per_id.feature_id);

    }
}

/**调用了fastPredictIMU(),用来预测最新P,V,Q的姿态
 * -latest_p,latest_q,latest_v，latest_acc_0,latest_gyr_0最新时刻的姿态。
 * 这个的作用是为了刷新姿态的输出，但是这个值的误差相对会比较大，是未经过非线性优化获取的初始值。
 * 
 * 让此时刻的值都等于上一时刻的值,用来更新状态
**/
void Estimator::fastPredictIMU(double t, Eigen::Vector3d linear_acceleration, Eigen::Vector3d angular_velocity)  //IMU中值积分，计算位姿与速度，注意此时的中值积分在世界坐标系下进行
{
    double dt = t - latest_time;
    latest_time = t;
    Eigen::Vector3d un_acc_0 = latest_Q * (latest_acc_0 - latest_Ba) - g;
    Eigen::Vector3d un_gyr = 0.5 * (latest_gyr_0 + angular_velocity) - latest_Bg;
    latest_Q = latest_Q * Utility::deltaQ(un_gyr * dt);
    Eigen::Vector3d un_acc_1 = latest_Q * (linear_acceleration - latest_Ba) - g;
    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);
    latest_P = latest_P + dt * latest_V + 0.5 * dt * dt * un_acc;
    latest_V = latest_V + dt * un_acc;
    latest_acc_0 = linear_acceleration;
    latest_gyr_0 = angular_velocity;
}

void Estimator::updateLatestStates()  //获取滑窗中最新帧时刻的状态，并在世界坐标系下进行中值积分；初始化完成后，最新状态在inputIMU函数中发布
{
    mPropagate.lock();
    latest_time = Headers[frame_count] + td;
    latest_P = Ps[frame_count];
    latest_Q = Rs[frame_count];
    latest_V = Vs[frame_count];
    latest_Ba = Bas[frame_count];
    latest_Bg = Bgs[frame_count];
    latest_acc_0 = acc_0;
    latest_gyr_0 = gyr_0;
    mBuf.lock();
    queue<pair<double, Eigen::Vector3d>> tmp_accBuf = accBuf;
    queue<pair<double, Eigen::Vector3d>> tmp_gyrBuf = gyrBuf;
    mBuf.unlock();
    while(!tmp_accBuf.empty())
    {
        double t = tmp_accBuf.front().first;
        Eigen::Vector3d acc = tmp_accBuf.front().second;
        Eigen::Vector3d gyr = tmp_gyrBuf.front().second;
        fastPredictIMU(t, acc, gyr);  //世界坐标系下进行中值积分
        tmp_accBuf.pop();
        tmp_gyrBuf.pop();
    }
    mPropagate.unlock();
}
