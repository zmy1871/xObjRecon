﻿#pragma once

#include <opencv2\opencv.hpp>
#include <vector>
#include <stack>

#include "Helpers/InnorealTimer.hpp"
#include "Helpers/xUtils.h"
#include "Helpers/UtilsMath.h"
#include "xObjTrack/Cuda/xObjTrackCudaFuncs.cuh"

#define PI 3.14159265f
#define MYEPS 1.0e-24

class xEdgeGen
{
public:
	xEdgeGen()
	{
		m_width = Resolution::getInstance().width();
		m_height = Resolution::getInstance().height();
		m_size = m_width * m_height;

#if USE_CPU
		float horizontal[9] = { -1, 0, 1, -2, 0, 2, -1, 0, 1 };
		m_filterH = cv::Mat(3, 3, CV_32FC1, horizontal) / 8.0f;
		m_filterV = m_filterH.t();
		m_dxDepth = cv::Mat(m_depthImg.size(), CV_32FC1);
		m_dyDepth = cv::Mat(m_depthImg.size(), CV_32FC1);
		m_magDepth = cv::Mat(m_depthImg.size(), CV_32FC1);
		m_mask = cv::Mat(m_depthImg.size(), CV_8UC1);
#endif
	
		checkCudaErrors(cudaMalloc((void **)&m_dResultBuf, (sizeof(float) * 2 + sizeof(int)) * 10));
		m_resultBuf = (char* )malloc((sizeof(float) * 2 + sizeof(int)) * 10);

		m_depthImgDevice = cv::cuda::GpuMat(m_height, m_width, CV_16UC1);
		m_depthImgFloatDevice = cv::cuda::GpuMat(m_height, m_width, CV_32FC1);
		m_depthImgBilateralDevice = cv::cuda::GpuMat(m_height, m_width, CV_32FC1);

		m_dEdgeIdx = cv::cuda::GpuMat(m_height, m_width, CV_32SC1);
		m_dEdge= cv::cuda::GpuMat(m_height, m_width, CV_8UC1);
		m_dMask = cv::cuda::GpuMat(m_height, m_width, CV_8UC1);
	
		m_edgeIdx = cv::Mat_<int>(m_height, m_width);
		m_boxVecTotal.reserve(200);

		m_validBox = Box(0, m_width, 0, m_height);
	}

	~xEdgeGen()
	{
		cudaFree(m_dResultBuf);
		free(m_resultBuf);
	}

	void calcValidBox(cv::Mat &depthImg, int leftShift = 0, int rightShift = 0, int bottomShift = 0)
	{
		m_validBox = Box(65535, 0, 65535, 0);
		for (int y = 0; y < depthImg.rows; ++y)
		{
			for (int x = 0; x < depthImg.cols; ++x)
			{
				if (depthImg.at<ushort>(y, x) > 0)
				{
					if (x < m_validBox.m_left) m_validBox.m_left = x;
					if (x > m_validBox.m_right) m_validBox.m_right = x;
					if (y < m_validBox.m_top) m_validBox.m_top = y;
					if (y > m_validBox.m_bottom) m_validBox.m_bottom = y;
				}
			}
		}
		m_validBox.m_left += leftShift, m_validBox.m_right += rightShift;
		m_validBox.m_bottom += bottomShift;
		std::cout << m_validBox.m_top << " : " << m_validBox.m_bottom << std::endl;
		std::cout << m_validBox.m_left << " : " << m_validBox.m_right << std::endl;
		//cv::rectangle(m_depthImg,
			//cv::Rect(m_validBox.m_left, m_validBox.m_top, m_validBox.m_right - m_validBox.m_left, m_validBox.m_bottom - m_validBox.m_top),
			//cv::Scalar(65525, 65525, 65525), 2);
		//cv::namedWindow("hehe");
		//cv::imshow("hehe", m_depthImg * 10);
		//cv::waitKey(0);
	}

#if USE_CPU
	void calcGradientCPU()
	{	
		cv::filter2D(m_depthImgBilateral, m_dyDepth, m_dyDepth.depth(), m_filterV);
		cv::filter2D(m_depthImgBilateral, m_dxDepth, m_dyDepth.depth(), m_filterH);

#pragma omp for
		for (int y = m_validBox.m_top; y < m_validBox.m_bottom; ++y)
		{
			for (int x = m_validBox.m_left; x < m_validBox.m_right; ++x)
			{	
				float dyVal, dxVal;
				dyVal = m_dyDepth(y, x);
				dxVal = m_dxDepth(y, x);
				m_magDepth(y, x) = sqrt(dyVal*dyVal + dxVal*dxVal);
			}
		}
	}
#endif

#if USE_CPU
	cv::Mat &getMask()
	{
		return m_mask;
	}
#endif

	cv::cuda::GpuMat &getMaskGpu()
	{
		return m_dMask;
	}	

	void calcEdge(cv::cuda::GpuMat& dFiltereddepthImg32F,
						 cv::cuda::GpuMat& dVMap,
						 Box& box,
						 float minDepth, float maxDepth, float magThreshold, bool withGravity)
	{
		CalcEdgeIdx(m_dEdgeIdx, m_dEdge, m_dMask, dFiltereddepthImg32F, dVMap, box,
		            minDepth, maxDepth, magThreshold * magThreshold,
		            m_validBox.m_left, m_validBox.m_right, m_validBox.m_top, m_validBox.m_bottom, withGravity);

		m_dEdgeIdx.download(m_edgeIdx);
#if 0
		//cv::Mat mask;
		//m_dMask.download(mask);
		//cv::imshow("mask", mask);
		cv::Mat filtereddepthImg32F;
		dFiltereddepthImg32F.download(filtereddepthImg32F);
		cv::imshow("filtereddepthImg32F", filtereddepthImg32F);
		cv::imshow("m_edgeIdx", m_edgeIdx * -1000000);
		cv::waitKey(0);
#endif
	}

	void calcEdgeOptWithBox(cv::cuda::GpuMat& dFiltereddepthImg32F,
							cv::cuda::GpuMat& dVMap,
							Box& box,
							float minDepth, float maxDepth, float magThreshold)
	{
		CalcEdgeIdxOptWithBox(m_dEdgeIdx, m_dEdge, m_dMask, dFiltereddepthImg32F, dVMap, box,
					minDepth, maxDepth, magThreshold * magThreshold);

		m_dEdgeIdx.download(m_edgeIdx);
#if 0
		cv::imshow("m_edgeIdx", m_edgeIdx * -1000000);
		cv::waitKey(0);
#endif
	}

	void calcEdgeOptWithoutBox(cv::cuda::GpuMat& dFiltereddepthImg32F,
							   cv::cuda::GpuMat& dVMap,
							   Box& box,
							   float minDepth, float maxDepth, float magThreshold) 
	{
		CalcEdgeIdxOptWithoutBox(m_dEdgeIdx, m_dEdge, m_dMask, dFiltereddepthImg32F, dVMap,
					minDepth, maxDepth, magThreshold * magThreshold,
					m_validBox.m_left, m_validBox.m_right, m_validBox.m_top, m_validBox.m_bottom);

		m_dEdgeIdx.download(m_edgeIdx);
#if 0
		cv::imshow("m_edgeIdx", m_edgeIdx * -1000000);
		cv::waitKey(0);
#endif
	}

	void calcEdge(cv::Mat &depthImg, Box &box, bool hasBox)
	{
#if 0
		m_depthImgDevice.upload(depthImg);
		m_depthImgDevice.convertTo(m_depthImgFloatDevice, CV_32FC1);
		cv::cuda::bilateralFilter(m_depthImgFloatDevice, m_depthImgBilateralDevice, 5, 5 * 2, 5 / 2);

		float minDepth = 0.0f, maxDepth = 1.0e24, magThreshold = 20.0f;;
		if (hasBox == true)
		{
			float meanDepth;
			cv::cuda::GpuMat &roi = m_depthImgBilateralDevice(cv::Rect(box.m_left, box.m_top, box.m_right - box.m_left, box.m_bottom - box.m_top)
				& cv::Rect(m_validBox.m_left, m_validBox.m_top, m_validBox.m_right - m_validBox.m_left, m_validBox.m_bottom - m_validBox.m_top));
			CalcMinMeanDepth(minDepth, meanDepth, roi, m_dResultBuf, m_resultBuf);
			minDepth = minDepth;
			maxDepth = meanDepth + 1.0 * (meanDepth - minDepth);
		}

		CalcEdgeIdx(m_dEdgeIdx, m_dMask, m_depthImgBilateralDevice, minDepth, maxDepth, magThreshold * magThreshold,
			m_validBox.m_left, m_validBox.m_right, m_validBox.m_top, m_validBox.m_bottom);

		m_dEdgeIdx.download(m_edgeIdx);	
#endif

#if 0
		cv::namedWindow("hehe1");
		cv::imshow("hehe1", m_edgeIdx * -1000000);
		cv::namedWindow("hehe2");
		cv::Mat mask;
		m_dMask.download(mask);
		cv::imshow("hehe2", mask);
		cv::waitKey(0);
#endif

#if USE_CPU
		m_depthImg.convertTo(m_depthImgFloat, CV_32FC1);
		cv::cuda::bilateralFilter(m_depthImgFloat, m_depthImgBilateral, 5, 5 * 2, 5 / 2);

		calcGradientCPU();

		cv::Mat &roi = m_depthImgBilateral(cv::Rect(box.m_left, box.m_top, box.m_right - box.m_left, box.m_bottom - box.m_top)
			& cv::Rect(m_validBox.m_left, m_validBox.m_top, m_validBox.m_right - m_validBox.m_left, m_validBox.m_bottom - m_validBox.m_top));
		float meanValDepth = 0.0, minValDepth = 1.0e32, maxValDepth;
		int cnt = 0;
		for (int y = 0; y < roi.rows; ++y)
		{
			for (int x = 0; x < roi.cols; ++x)
			{
				if (roi.at<float>(y, x) > EPS)
				{
					meanValDepth += roi.at<float>(y, x);
					++cnt;
					if (minValDepth > roi.at<float>(y, x))
					{
						minValDepth = roi.at<float>(y, x);
					}
				}
			}
		}
		meanValDepth /= cnt;
		minValDepth = minValDepth - 1.0;
		maxValDepth = meanValDepth + 1.0 * (meanValDepth - minValDepth) + 1.0;
		//std::cout << "min: " << minValDepth << std::endl;
		//std::cout << "max: " << maxValDepth << std::endl;
		//std::cout << "mean: " << meanValDepth << std::endl;			

		memset(m_edgeInd.data, 0, m_size * sizeof(int));
		memset(m_mask.data, 0, m_size * sizeof(uchar));
#pragma omp for
		for (int y = m_validBox.m_top; y < m_validBox.m_bottom; ++y)
		{
			for (int x = m_validBox.m_left; x < m_validBox.m_right; ++x)
			{
				float depth = m_depthImgBilateral.at<float>(y, x);

				if (depth >= minValDepth && depth <= maxValDepth)
				{
					m_mask.at<uchar>(y, x) = 255;

					if (m_magDepth(y, x) > 20.0f)
						m_edgeInd(y, x) = -1;
				}
			}
		}
#endif
#if 0
		cv::namedWindow("hehe");
		cv::imshow("hehe", m_edgeInd * -100000);
		cv::waitKey(0);
#endif
	}

	void connectedComponent(int &ccNum, Box &box, int rows, int cols)
	{
		int label = 0; // start by 1  
		std::stack<std::pair<int, int> > neighborPixels;
		for (int r = box.m_top; r < box.m_bottom; ++r)
		//for (int r = 0; r < rows; ++r)
		{
			for (int c = box.m_left; c < box.m_right; ++c)
			//for (int c = 0; c < cols; ++c)
			{
				if (m_edgeIdx.at<int>(r, c) == -1)
				{	
					neighborPixels.push(std::pair<int, int>(r, c));
					++label;
					while (!neighborPixels.empty())
					{
						// get the top pixel on the stack and label it with the same label  
						std::pair<int, int> &curPixel = neighborPixels.top();
						int curRow = curPixel.first;
						int curCol = curPixel.second;
						m_edgeIdx.at<int>(curRow, curCol) = label;

						// pop the top pixel  
						neighborPixels.pop();

						if (curRow == 0 || curCol == 0 || curRow == m_height - 1 || curCol == m_width - 1)
						{
							m_edgeIdx.at<int>(curRow, curCol) = 0;
							continue;
						}
						// push the 8-neighbors (foreground pixels)  
						if (m_edgeIdx.at<int>(curRow, curCol - 1) == -1)
						{// left pixel
							neighborPixels.push(std::pair<int, int>(curRow, curCol - 1));
							m_edgeIdx.at<int>(curRow, curCol - 1) = 0;
						}
						if (m_edgeIdx.at<int>(curRow, curCol + 1) == -1)
						{// right pixel
							neighborPixels.push(std::pair<int, int>(curRow, curCol + 1));
							m_edgeIdx.at<int>(curRow, curCol + 1) = 0;
						}
						if (m_edgeIdx.at<int>(curRow - 1, curCol) == -1)
						{// up pixel
							neighborPixels.push(std::pair<int, int>(curRow - 1, curCol));
							m_edgeIdx.at<int>(curRow - 1, curCol) = 0;
						}
						if (m_edgeIdx.at<int>(curRow + 1, curCol) == -1)
						{// down pixel
							neighborPixels.push(std::pair<int, int>(curRow + 1, curCol));
							m_edgeIdx.at<int>(curRow + 1, curCol) = 0;
						}

						if (m_edgeIdx.at<int>(curRow - 1, curCol - 1) == -1)
						{
							neighborPixels.push(std::pair<int, int>(curRow - 1, curCol - 1));
							m_edgeIdx.at<int>(curRow - 1, curCol - 1) = 0;
						}
						if (m_edgeIdx.at<int>(curRow - 1, curCol + 1) == -1)
						{
							neighborPixels.push(std::pair<int, int>(curRow - 1, curCol + 1));
							m_edgeIdx.at<int>(curRow - 1, curCol + 1) = 0;
						}
						if (m_edgeIdx.at<int>(curRow + 1, curCol - 1) == -1)
						{
							neighborPixels.push(std::pair<int, int>(curRow + 1, curCol - 1));
							m_edgeIdx.at<int>(curRow + 1, curCol - 1) = 0;
						}
						if (m_edgeIdx.at<int>(curRow + 1, curCol + 1) == -1)
						{
							neighborPixels.push(std::pair<int, int>(curRow + 1, curCol + 1));
							m_edgeIdx.at<int>(curRow + 1, curCol + 1) = 0;
						}
					}
				}
			}
		}

		ccNum = label;
	}

#if 0
	{
		//cv::Mat_<int> edge;
		cv::Mat_<uchar> edgeColor;

		//cv::Mat_<float> orientation = cv::Mat_<float>::zeros(imgColor.size());
		//cv::Mat_<float>	magnitude = cv::Mat_<float>::zeros(imgColor.size());

		std::vector<std::vector<int> > keyPoints;
		connectedComponent(ccNum, keyPoints, box, type);

#if 0
		cv::Mat ccImgVis = cv::Mat::zeros(480, 640, CV_8UC3);
		for (int r = 0; r < ccImgVis.rows; r++)
		{
			for (int c = 0; c < ccImgVis.cols; c++)
			{
				int i = m_edgeInd(r, c);
				if (i > 0)
				{
					ccImgVis.at<cv::Vec3b>(r, c)[0] = i <= 0 ? 1 : (123 * i + 128) % 255;
					ccImgVis.at<cv::Vec3b>(r, c)[1] = i <= 0 ? 1 : (7 * i + 3) % 255;
					ccImgVis.at<cv::Vec3b>(r, c)[2] = i <= 0 ? 1 : (1174 * i + 80) % 255;
				}
			}
		}
		cv::namedWindow("hehehe");
		cv::imshow("hehehe", ccImgVis);
		cv::waitKey(1);
#endif

		return;

#if 0
		float maxMag = 0.0;
		for (int y = 0; y < edge.rows; ++y)
		{
			for (int x = 0; x < edge.cols; ++x)
			{
				dyVal = m_dyColor(y, x) + EPS;
				dxVal = m_dxColor(y, x) + EPS;
				orientation(y, x) = atan2(dyVal, dxVal);
				if (orientation(y, x) < 0) orientation(y, x) += PI;
				magnitude(y, x) += sqrt(dyVal*dyVal + dxVal*dxVal);
				if (maxMag < magnitude(y, x))
					maxMag = magnitude(y, x);
			}
		}
#endif
		// Note: Using SIFT, magnitude can not be scaled
		// magnitude = magnitude * (1.0 / maxMag);

#if 0
		siftKeyPoints.clear();
		SiftGPU::SiftKeypoint siftKeyPoint;
		keyPointNumPerCC.resize(keyPoints.size());
		for (int i = 0; i < keyPoints.size(); ++i)
		{
			keyPointNumPerCC[i] = keyPoints[i].size();
			for (int j = 0; j < keyPoints[i].size(); ++j)
			{
				int x = keyPoints[i][j] % m_width;
				int y = keyPoints[i][j] / m_width;
				siftKeyPoint.x = x;
				siftKeyPoint.y = y;
				//siftKeyPoint.s = magnitude(y, x);
				siftKeyPoint.o = orientation(y, x);
				siftKeyPoints.push_back(siftKeyPoint);
			}
		}
#endif
	}
#endif

	void getBoxes(cv::Mat &colorImg, std::vector<Box> &boxVec)
	{
		int ccNum;
		connectedComponent(ccNum, m_validBox, colorImg.rows, colorImg.cols);
	
#if 0
		cv::Mat ccImgVis = cv::Mat::zeros(m_edgeIdx.size(), CV_8UC3);
		for (int r = 0; r < ccImgVis.rows; r++)
		{
			for (int c = 0; c < ccImgVis.cols; c++)
			{
				int i = m_edgeIdx(r, c);
				if (i > 0)
				{
					ccImgVis.at<cv::Vec3b>(r, c)[0] = i <= 0 ? 1 : (123 * i + 128) % 255;
					ccImgVis.at<cv::Vec3b>(r, c)[1] = i <= 0 ? 1 : (7 * i + 3) % 255;
					ccImgVis.at<cv::Vec3b>(r, c)[2] = i <= 0 ? 1 : 255;// (1174 * i + 80) % 255;
				}
			}
		}	
#if 1
		cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
		cv::dilate(ccImgVis, ccImgVis, kernel);
		for (int r = 0; r < ccImgVis.rows; r++)
		{
			for (int c = 0; c < ccImgVis.cols; c++)
			{
				if (ccImgVis.at<cv::Vec3b>(r, c)[0] > 0)
				{
					colorImg.at<cv::Vec3b>(r, c)[0] = ccImgVis.at<cv::Vec3b>(r, c)[0];
					colorImg.at<cv::Vec3b>(r, c)[1] = ccImgVis.at<cv::Vec3b>(r, c)[1];
					colorImg.at<cv::Vec3b>(r, c)[2] = ccImgVis.at<cv::Vec3b>(r, c)[2];
				}
			}
		}
		cv::imshow("object detect cc", ccImgVis);
		cv::waitKey(1);
#endif
#endif
#if 0
		char renderedDir[256];
		cv::namedWindow("object detect cc");
		cv::imshow("object detect cc", ccImgVis);
		std::vector<int> pngCompressionParams;
		pngCompressionParams.push_back(CV_IMWRITE_PNG_COMPRESSION);
		pngCompressionParams.push_back(0);
		sprintf(renderedDir, "D:\\xjm\\result\\for_demo\\test\\%06d.png", 0);
		cv::imwrite(renderedDir, colorImg, pngCompressionParams);
		cv::waitKey(0);
		std::exit(0);
#endif

		m_boxVecTotal.resize(ccNum);
		m_numVec.resize(ccNum, 0);
		for (int i = 0; i < m_boxVecTotal.size(); ++i)
		{
			m_boxVecTotal[i].m_left = m_width;
			m_boxVecTotal[i].m_right = 0;
			m_boxVecTotal[i].m_top = m_height;
			m_boxVecTotal[i].m_bottom = 0;
		}
		int ind;
		for (int r = 0; r < m_height; r++)
		{
			for (int c = 0; c < m_width; c++)
			{
				ind = m_edgeIdx.at<int>(r, c) - 1;
				if (ind >= 0)
				{
					++m_numVec[ind];
					Box &tmp = m_boxVecTotal[ind];
					// pad 3 pixel
					if (r < tmp.m_top) tmp.m_top = r - 3;
					if (c < tmp.m_left) tmp.m_left = c - 3;
					if (r > tmp.m_bottom) tmp.m_bottom = r + 3;
					if (c > tmp.m_right) tmp.m_right = c + 3;	
				}
			}
		}

		boxVec.clear();	
		for (int i = 0; i < m_boxVecTotal.size(); ++i)
		{
			Box &tmp = m_boxVecTotal[i];
			if ((tmp.m_bottom - tmp.m_top) > 10 &&
				(tmp.m_right - tmp.m_left > 10) &&
				m_numVec[i] > 50)
			{ 
				m_boxVecTotal[i].m_top = clamp(m_boxVecTotal[i].m_top, 0, m_height - 1);
				m_boxVecTotal[i].m_left = clamp(m_boxVecTotal[i].m_left, 0, m_width - 1);
				m_boxVecTotal[i].m_bottom = clamp(m_boxVecTotal[i].m_bottom, 0, m_height - 1);
				m_boxVecTotal[i].m_right = clamp(m_boxVecTotal[i].m_right, 0, m_width - 1);
				boxVec.push_back(m_boxVecTotal[i]);
			}
		}
	}

	void getObjectBox(Box &objectBox, std::vector<Box> &boxVec)
	{
		int centerX = m_width / 2, centerY = m_height / 2, area = m_size;
		objectBox = Box(0, 0, 0, 0);
		for (int i = 0; i < boxVec.size(); ++i)
		{
			Box &tmp = boxVec[i];
			if (centerY > tmp.m_top && centerY < tmp.m_bottom && centerX > tmp.m_left && centerX < tmp.m_right
				&& (tmp.m_bottom - tmp.m_top) * (tmp.m_right - tmp.m_left) < area)
			{
				objectBox = boxVec[i];
				objectBox.m_score = fabs(centerX - (tmp.m_right + tmp.m_left) / 2)
					+ fabs(centerY - (tmp.m_bottom + tmp.m_top) / 2);
				area = (tmp.m_bottom - tmp.m_top) * (tmp.m_right - tmp.m_left);
#if 0
				std::cout << "center: " << centerX << ", " << centerY << std::endl;
				std::cout << "object box: " << objectBox.m_top << ", " 
					<< objectBox.m_bottom << ", " 
					<< objectBox.m_left << ", " 
					<< objectBox.m_right << ", " << std::endl;
#endif
			}
		}
	}

public:
#if USE_CPU
	cv::Mat m_filterH, m_filterV;
	cv::Mat_<float> m_dxDepth, m_dyDepth, m_magDepth;

	cv::Mat m_depthImgFloat, m_depthImgBilateral;
	cv::Mat m_mask;
#endif

	char* m_dResultBuf, *m_resultBuf;

	cv::cuda::GpuMat m_dEdgeIdx, m_dEdge;
	cv::Mat_<int> m_edgeIdx;	
	cv::cuda::GpuMat m_dMask;
	
	cv::cuda::GpuMat m_depthImgDevice, m_depthImgFloatDevice, m_depthImgBilateralDevice;
	int m_width, m_height, m_size;
	Box m_validBox;

	std::vector<Box> m_boxVecTotal;
	std::vector<int> m_numVec;
};

