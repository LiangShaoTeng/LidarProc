/*
 * @Descripttion: 
 * @version: 1.0版本
 * @Author: Frank.Wu
 * @Date: 2020-01-09 15:25:57
 * @LastEditors: Frank.Wu
 * @LastEditTime: 2020-03-05 18:47:44
 */
#ifdef _USE_PCL_
#include "LidarFeaturePoints.h"
#include "../LidarGeometry/Geometry.h"

#include <boost/thread/thread.hpp>
#include <pcl/range_image/range_image.h>
#include <pcl/features/range_image_border_extractor.h>

#include <pcl/console/time.h>
#include <pcl/point_types.h>
#include <pcl/common/io.h>
#include <pcl/keypoints/sift_keypoint.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/fpfh.h>
#include <pcl/kdtree/kdtree_flann.h>

#include<ceres/ceres.h>
using namespace ceres;
using namespace GeometryLas;

typedef pcl::PointXYZ PointType;
/**
 * @brief  重构一下取值函数，默认取值为去强度值，没有这个部分则报错：
           error: ‘const struct pcl::PointXYZ’ has no member named ‘intensity’
 * @note   
 * @retval None
 */
namespace pcl
{
  template<>
    struct SIFTKeypointFieldSelector<PointXYZ>
    {
      inline float
      operator () (const PointXYZ &p) const
      {
    return p.z;
      }
    };
}

/**
 * @brief  代码运行过程会有内存泄露的问题，感觉应该是库的问题，但是不知道问题在哪里
            经过检查是RangeImage这段代码的问题，感觉跟内核有关，估计是虚拟机运行
            不支持，或者是指令集的问题
            2019-07-21:经过查看资料发现是PCL库编译的问题，存在Boost库与STD冲突的问题
            编译的时候添加C++11标准用Release编译就可以解决这个问题
            2019-12-26:一直提取特征Narf点数量为0，不知道问题出现在哪里,需要进一步研究
 * @note   
 * @param  input_cloud:  输入点云
 * @param  &narfIndex:   特征点点云
 * @param  &narfDesc:    特征点描述
 * @retval 
 */
long LidarFeaturePoints::LidarFeature_NARF(pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud,pcl::PointCloud<int> &narfIndex,pcl::PointCloud<pcl::Narf36> &narfDesc)
{
    pcl::PointCloud<PointType>& point_cloud = *input_cloud;
    float angular_resolution = 0.5f;           ////angular_resolution为模拟的深度传感器的角度分辨率，即深度图像中一个像素对应的角度大小
    float support_size = 4.0f;                 //点云大小的设置
    pcl::RangeImage::CoordinateFrame coordinate_frame = pcl::RangeImage::CAMERA_FRAME;     //设置坐标系
    bool setUnseenToMaxRange = false;
    bool rotation_invariant = true;
    pcl::PointCloud<pcl::PointWithViewpoint> far_ranges;
    Eigen::Affine3f scene_sensor_pose (Eigen::Affine3f::Identity ());
    
    // -----------------------------------------------
	// -----Create RangeImage from the PointCloud-----
	// -----------------------------------------------
    float noise_level = 0.0;
    float min_range = 0.0f;
    int border_size = 1;
    boost::shared_ptr<pcl::RangeImage> range_image_ptr (new pcl::RangeImage());
    pcl::RangeImage& range_image = *range_image_ptr;  
    range_image.createFromPointCloud (point_cloud, angular_resolution, pcl::deg2rad (360.0f), pcl::deg2rad (180.0f),
                                   scene_sensor_pose, coordinate_frame, noise_level, min_range, border_size);
    range_image.integrateFarRanges (far_ranges); 

    if (setUnseenToMaxRange)
        range_image.setUnseenToMaxRange ();

    /*提取特征点*/
    pcl::RangeImageBorderExtractor range_image_border_extractor;   //用来提取边缘
    pcl::NarfKeypoint narf_keypoint_detector;      //用来检测关键点
    narf_keypoint_detector.setRangeImageBorderExtractor (&range_image_border_extractor);   //
    narf_keypoint_detector.setRangeImage (&range_image);
    narf_keypoint_detector.getParameters ().support_size = support_size;    //设置NARF的参数
    narf_keypoint_detector.compute (narfIndex);

    /*提取特征描述*/
    std::vector<int> keypoint_indices2;
    keypoint_indices2.resize (narfIndex.points.size ());
    for (unsigned int i=0; i<narfIndex.size (); ++i) // This step is necessar_t[1] to get the right vector type
        keypoint_indices2[i]=narfIndex.points[i];
    pcl::NarfDescriptor narf_descriptor (&range_image, &keypoint_indices2);
    narf_descriptor.getParameters ().support_size = support_size;
    narf_descriptor.getParameters ().rotation_invariant = rotation_invariant;
    narf_descriptor.compute (narfDesc);
    printf("feature points count:%d\n",narfIndex.size());
    return 0;
}

long LidarFeaturePoints::LidarFeature_Sift(pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud,
                                           pcl::PointCloud<int> &siftPointIdx,
                                           pcl::PointCloud<pcl::FPFHSignature33>::Ptr siftFPFH)
{
    // Parameters for sift computation
    float min_scale = 1.0f;       //the standard deviation of the smallest scale in the scale space
    int n_octaves   = 8;          //the number of octaves (i.e. doublings of scale) to compute
    int n_scales_per_octave = 4;  //the number of scales to compute within each octave
    float min_contrast = 0.2f;    //the minimum contrast required for detection

    pcl::console::TicToc time;
    time.tic();

    // Estimate the sift interest points using z values from xyz as the Intensity variants
    //pcl::PointCloud<pcl::PointXYZ>::Ptr feauture_cloud
    pcl::SIFTKeypoint<pcl::PointXYZ, pcl::PointWithScale> sift;
    pcl::PointCloud<pcl::PointWithScale> result;
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ> ());

    sift.setSearchMethod(tree);
    sift.setScales(min_scale, n_octaves, n_scales_per_octave);
    sift.setMinimumContrast(min_contrast);
    sift.setInputCloud(input_cloud);
    sift.compute(result);

    //pcl::PointIndicesConstPtr keypoints_indices = sift.getKeypointsIndices();
    //计算点云特征
    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>());
    pcl::NormalEstimation<pcl::PointXYZ,pcl::Normal> est_normal;
    est_normal.setInputCloud(input_cloud);
    est_normal.setSearchMethod(tree);
    est_normal.setKSearch(10);          //近邻10个点
    est_normal.compute(*normals);       //计算法线

    //创建FPFH估计对象fpfh，并把输入数据集cloud和法线normals传递给它。
    pcl::FPFHEstimation<pcl::PointXYZ,pcl::Normal,pcl::FPFHSignature33> fpfh;
    pcl::search::KdTree<pcl::PointXYZ>::Ptr treeFPFH(new pcl::search::KdTree<pcl::PointXYZ> ());
    fpfh.setInputCloud(input_cloud);
    fpfh.setInputNormals(normals);
    fpfh.setSearchMethod(treeFPFH);
    //fpfh.setIndices(keypoints_indices);
    pcl::PointCloud<pcl::FPFHSignature33>::Ptr fpfhs(new pcl::PointCloud<pcl::FPFHSignature33>());
    fpfh.setKSearch(10);        //近邻10个点
    fpfh.compute(*fpfhs);       //计算特征
    
    //判断SIFT点在原始点云中的index
    const int searchNum=1;
    std::vector<int> pointIdxNKNSearch(searchNum);
    std::vector<float> pointNKNSquaredDistance(searchNum);
    pcl::KdTreeFLANN<pcl::PointXYZ> kdtree;
    kdtree.setInputCloud(input_cloud);
    for(auto resPoint : result.points)
    {
        pcl::PointXYZ searchPoint;
        searchPoint.x=resPoint.x;
        searchPoint.y=resPoint.y;
        searchPoint.z=resPoint.z;

        if (kdtree.nearestKSearch(searchPoint, searchNum, pointIdxNKNSearch, pointNKNSquaredDistance) > 0 )
        {
            siftPointIdx.push_back(pointIdxNKNSearch[0]);
            siftFPFH->push_back(fpfhs->points[pointIdxNKNSearch[0]]);
        }
    }

    std::cout << "Computing Points: "<<input_cloud->size()<<std::endl;
    std::cout << "Computing FPFH Features: "<<siftFPFH->size()<<std::endl;
    std::cout << "Computing the SIFT points takes "<<time.toc()/1000<<"seconds"<<std::endl;
    std::cout << "No. of SIFT points in the result are " << siftPointIdx.size () << std::endl;

/*  std::cout << "Computing Points: "<<input_cloud->size()<<std::endl;
    std::cout << "Computing FPFH Features: "<<fpfhs->size()<<std::endl;
    std::cout << "Computing Normals: "<<normals->size()<<std::endl;
    std::cout << "Computing the SIFT points takes "<<time.toc()/1000<<"seconds"<<std::endl;
    std::cout << "No. of SIFT points in the result are " << result.points.size () << std::endl;
 */
    // Copying the pointwithscale to pointxyz so as visualize the cloud
    //copyPointCloud(result, *feauture_cloud);
    //std::cout << "SIFT points in the result are " << feauture_cloud->points.size () << std::endl;

#ifdef _DEBUG
    FILE *fs = fopen("../data/test/sift.txt","w+");
    if(fs!=nullptr)
    {
        for(int i=0;i<siftPointIdx.points.size ();++i)
            fprintf(fs,"%lf  %lf  %lf\n",input_cloud->points[siftPointIdx[i]].x,
                                        input_cloud->points[siftPointIdx[i]].y,
                                        input_cloud->points[siftPointIdx[i]].z);
        fclose(fs);
    }else
    {
        printf("create test output failed!\n");
    }
#endif
    return 0;
}

// 直接通过kdtree查找比较快
// inline double disFpfh(pcl::FPFHSignature33 pt1,pcl::FPFHSignature33 pt2)
// {
//     double dis = 0;
//     for(int i=0;i<pt1.descriptorSize();++i)
//     {
//         dis+=(pt1.histogram[i]-pt2.histogram[i])*(pt1.histogram[i]-pt2.histogram[i]);
//     }
//     return sqrt(dis);

// }

long LidarFeatureRegistration::LidarRegistration_Sift(pcl::PointCloud<pcl::FPFHSignature33>::Ptr siftFPFH1,
                                pcl::PointCloud<pcl::FPFHSignature33>::Ptr siftFPFH2,
                                pcl::PointCloud<int> &siftMatchPointIdx)
{
    pcl::KdTreeFLANN<pcl::FPFHSignature33> kdtree;
    kdtree.setInputCloud(siftFPFH1);
    const int searchNum=1;
    std::vector<int> pointIdxNKNSearch(searchNum);
    std::vector<float> pointNKNSquaredDistance(searchNum);
    
    //特征匹配
    int num=0;
    for(auto fpfhTar:siftFPFH2->points)
    {
        if (kdtree.nearestKSearch(fpfhTar, searchNum, pointIdxNKNSearch, pointNKNSquaredDistance) > 0 )
        {
            siftMatchPointIdx.push_back(pointIdxNKNSearch[0]);
            num++;
            //printf("%d-%d\n",num,pointIdxNKNSearch[0]);
        }
    }
}
#ifdef _USE_CERES_
/**
 * @name: ceres 自动求导,注意，各个库采用的编译器类型要一致，
 *        PCL采用C++14 则ceres也采用c++14，否则在link的时候会出问题
 * @msg: 
 * @return: 
 */
struct CostFunctorRotTrans{
    
	CostFunctorRotTrans(double x1,double y1,double z1,double x2,double y2,double z2)
                        :m_x1(x1), m_y1(y1), m_z1(z1), m_x2(x2), m_y2(y2), m_z2(z2)
    {}

	template <typename T> 
    bool operator()(const T* r_t, T* residual) const 
    {
        T a11 = cos(r_t[1])*cos(r_t[2]);
        T a12 = sin(r_t[0])*sin(r_t[1])*cos(r_t[2]) - cos(r_t[0])*sin(r_t[2]);
        T a13 = cos(r_t[0])*sin(r_t[1])*cos(r_t[2]) + sin(r_t[0])*sin(r_t[2]);
        T b11 = cos(r_t[1])*sin(r_t[2]);
        T b12 = sin(r_t[0])*sin(r_t[1])*sin(r_t[2]) + cos(r_t[0])*cos(r_t[2]);
        T b13 = cos(r_t[0])*sin(r_t[1])*sin(r_t[2]) - sin(r_t[0])*cos(r_t[2]);
        T c11 = -sin(r_t[1]);
        T c12 = sin(r_t[0])*cos(r_t[1]);
        T c13 = cos(r_t[0])*cos(r_t[1]);

        residual[0]=(a11*m_x1+a12*m_y1+a13*m_z1+r_t[3]-m_x2)*
                    (a11*m_x1+a12*m_y1+a13*m_z1+r_t[3]-m_x2);
        residual[1]=(b11*m_x1+b12*m_y1+b13*m_z1+r_t[4]-m_y2)*
                    (b11*m_x1+b12*m_y1+b13*m_z1+r_t[4]-m_y2);
        residual[2]=(c11*m_x1+c12*m_y1+c13*m_z1+r_t[5]-m_z2)*
                    (c11*m_x1+c12*m_y1+c13*m_z1+r_t[5]-m_z2);
		
        return true;
	}
 
	// static ceres::CostFunction* Create(const double observed_depth) {
	// 	return (new ceres::AutoDiffCostFunction<CostFunctorRotTrans, 6, 3>(
	// 		new CostFunctorRotTrans(m_obs,m_match)));
	// }
 
    const double m_x1;
    const double m_y1;
    const double m_z1;
    const double m_x2;
    const double m_y2;
    const double m_z2;
};

/**
 * @name: 获取数值中点
 * @param {type} 
 * @return: 
 */
static Point3D GetMaxMinCenterPoint(pcl::PointCloud<pcl::PointXYZ>::Ptr pts_cloud)
{
    Point3D ptMax(-999999999,-999999999,-999999999);
    Point3D ptMin(999999999,999999999,999999999);

    for (int i = 0; i < pts_cloud->points.size(); ++i) 
    {
        ptMax.x=max(double(pts_cloud->points[i].x),ptMax.x);
        ptMax.y=max(double(pts_cloud->points[i].y),ptMax.y);
        ptMax.z=max(double(pts_cloud->points[i].z),ptMax.z);

        ptMin.x=min(double(pts_cloud->points[i].x),ptMin.x);
        ptMin.y=min(double(pts_cloud->points[i].y),ptMin.y);
        ptMin.z=min(double(pts_cloud->points[i].z),ptMin.z);
    }

    return Point3D((ptMax.x+ptMin.x)/2,(ptMax.y+ptMin.y)/2,(ptMax.z+ptMin.z)/2);
}

//TODO:
//功能是做完了，最后也能够收敛，但是计算结果的残差太大，而且在匹配过程中可能存在误匹配的店
//第一步是需要处理误匹配的店，然后再找到减小残差的办法
long LidarFeatureRegistration::LidarRegistration_RotTrans(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud1,
                                    pcl::PointCloud<int> ptSiftIdx1,
                                    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud2,
                                    pcl::PointCloud<int> ptSiftIdx2,
                                    pcl::PointCloud<int> siftMatchPointIdx,
                                    double *r_t)
{
    Problem problem;
    for(int i=0;i<6;++i)
        r_t[i]=0;
    Point3D ptCent1 = GetMaxMinCenterPoint(cloud1);
    Point3D ptCent2 = GetMaxMinCenterPoint(cloud2);
    
    double *obs=new double[3*siftMatchPointIdx.points.size()];
    double *match=new double[3*siftMatchPointIdx.points.size()];

    for (int i = 0; i < siftMatchPointIdx.points.size(); ++i) 
    {
        const int idx1 = ptSiftIdx2.points[i];
        const int idx2 = ptSiftIdx1.points[siftMatchPointIdx.points[i]];
        
        obs[i*3+0]=cloud1->points[idx1].x-ptCent1.x;
        obs[i*3+1]=cloud1->points[idx1].y-ptCent1.y;
        obs[i*3+2]=cloud1->points[idx1].z-ptCent1.z;

        match[i*3+0]=cloud2->points[idx2].x-ptCent1.x;
        match[i*3+1]=cloud2->points[idx2].y-ptCent1.y;
        match[i*3+2]=cloud2->points[idx2].z-ptCent1.z;
    }

    for (int i = 0; i < siftMatchPointIdx.points.size(); ++i) 
    {
        CostFunction* cost_function =
            new AutoDiffCostFunction<CostFunctorRotTrans, 3,6>(
                new CostFunctorRotTrans(obs[i+3+0],obs[i+3+1],obs[i+3+2],match[i*3+0],match[i*3+1],match[i*3+2]));
        problem.AddResidualBlock(cost_function, nullptr, r_t);
    }
    
    Solver::Options options;
    options.linear_solver_type=ceres::DENSE_QR;
    options.minimizer_progress_to_stdout=true;
    options.max_num_iterations = 500;
    Solver::Summary summary;
    Solve(options,&problem,&summary);

    delete obs;obs=nullptr;
    delete match;match=nullptr;

    return 0;
}
#endif

#endif