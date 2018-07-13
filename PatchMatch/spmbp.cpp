﻿
#include "spmbp.h"
#include "stdlib.h"
#include "string.h"
#include "math.h"
#include "time.h"
#include "omp.h"
#include <vector>
#include "opencv2/opencv.hpp"
#include "CF_Filter_Header.h"
#include "FlowInputOutput.h"
#include "Superpixels_Header.h"
#include "colorcode.h"

using std::vector;
using std::min;
using std::max;

spm_bp::spm_bp(int positive)
{
	this->positive = positive;
}

spm_bp::~spm_bp(void)
{
}

void spm_bp::loadPairs(cv::Mat& in1, Mat& in2)
{
    in1.copyTo(im1d);
    in2.copyTo(im2d);
    im1d.convertTo(im1f, CV_32FC3);
    im2d.convertTo(im2f, CV_32FC3);
    width1 = im1f.cols;
    height1 = im1f.rows;
    width2 = im2f.cols;
    height2 = im2f.rows;
    // srand(time(NULL));
    // printf("Setting random seed\n");
    // srand(813867);
}

void spm_bp::setParameters(spm_bp_params* params)
{
    //** Parameters 41
    disp_range_u = params->max_u; //vertical //200
    disp_range_v = params->max_v; //horizontal //200
    range_u = 2 * disp_range_u + 1;
    range_v = 2 * disp_range_v + 1;
    iterNum = params->iter_num; //6
    alpha = params->alpha;      //weight for gradient components
    tau_c = params->tau_c;
    tau_s = params->tau_s;
    upScale = params->up_rate;
    lambda_smooth = params->lambda;

    //super pixel
    g_spMethod = 0;
    g_spNumber = params->sp_num;
    g_spSize = params->sp_num;
    g_spSizeOrNumber = 1;

    //filter kernel
    g_filterKernelSize = params->kn_size;
    g_filterKernelBoundarySize = 2 * g_filterKernelSize;
    g_filterKernelColorTau = params->kn_tau;
    g_filterKernelEpsl = params->kn_epsl;

    display = params->display;
}

inline bool checkEqual_PMF_PMBP(Vec2f v1, Vec2f v2)
{
    return (fabs(v1[0] - v2[0]) <= EPS) && (fabs(v1[1] - v2[1]) <= EPS);
}

inline int isNewLabel_PMF_PMBP(const vector<Vec2f>& vec_label, const Vec2f& test_label)
{
    int vec_size = vec_label.size();
    for (int m = 0; m < vec_size; m++) {
        if (checkEqual_PMF_PMBP(test_label, vec_label[m]))
            return m;
    }
    return -1;
}

inline float ComputeMes_PMBP_per_label(const float* dis_belief,
                                       const Mat_<Vec2f>& label_k,
                                       int p, 
                                       const Vec2f& disp_ref,
                                       float wt, float tau_s)
{
    float min_cost = 1e5;
    for (int k = 0; k < NUM_TOP_K; ++k) {
#if SMOOTH_COST_TRUNCATED_L1
        float cost_tp = dis_belief[k] + wt * min(float(abs(disp_ref[0] - label_k[p][k][0]) + abs(disp_ref[1] - label_k[p][k][1])), tau_s);
#endif

#if SMOOTH_COST_TRUNCATED_L2
        float cost_tp = dis_belief[k] + wt * min((float)(pow(disp_ref[0] - label_k[p][k][0], 2) +
                                                         pow(disp_ref[1] - label_k[p][k][1], 2)),
                                                  tau_s);
#endif
        if (cost_tp < min_cost)
            min_cost = cost_tp;
    }
    return min_cost;
}

inline void Compute_top_k(
        vector<float>& vec_belief,
        vector<Vec2f>& vec_label,
        vector<float>& vec_mes_l, vector<float>& vec_mes_r, vector<float>& vec_mes_u, vector<float>& vec_mes_d,
        vector<float>& vec_d_cost,
        Mat_<Vec<float, NUM_TOP_K> >& mes_pixel,
        Mat_<Vec2f>& label_pixel,
        Mat_<float>& d_cost,
        int p, int num_top_k)
{
    size_t vec_in_size = vec_belief.size();
    for (int i = 0; i < num_top_k; i++) {
        float belief_min = vec_belief[i];
        int id = i;
        for (size_t j = i + 1; j < vec_in_size; j++) {
            if (vec_belief[j] < belief_min) {
                belief_min = vec_belief[j];
                id = j;
            }
        }

        if (! vec_mes_l.empty())
            mes_pixel[p][0][i] = vec_mes_l[id];
        if (! vec_mes_r.empty())
            mes_pixel[p][1][i] = vec_mes_r[id];
        if (! vec_mes_u.empty())
            mes_pixel[p][2][i] = vec_mes_u[id];
        if (! vec_mes_d.empty())
            mes_pixel[p][3][i] = vec_mes_d[id];

        label_pixel[p][i] = vec_label[id];
        d_cost[p][i] = vec_d_cost[id];

        vec_belief[id] = vec_belief[i];
        if (! vec_mes_l.empty())
            vec_mes_l[id] = vec_mes_l[i];
        if (! vec_mes_r.empty())
            vec_mes_r[id] = vec_mes_r[i];
        if (! vec_mes_u.empty())
            vec_mes_u[id] = vec_mes_u[i];
        if (! vec_mes_d.empty())
            vec_mes_d[id] = vec_mes_d[i];

        vec_label[id] = vec_label[i];
        vec_d_cost[id] = vec_d_cost[i];
    }
}

void Message_normalization_PMF_PMBP(Mat_<Vec<float, NUM_TOP_K> >& mes_pixel, int p, int num_top_k)
{
    // TODO: Try to parallelize this using reduction?
    // #pragma omp parallel for
    for (int i = 0; i < 4; i++) {
        float val = 0.0;

        // TODO: How many elements are there in mes_pixel? NUM_TOP_K?
        // If that is the case we can just sum over vector to get val
        // We can also do loop unrolling directly
        for (int k = 0; k < num_top_k; k++)
            val += mes_pixel[p][i][k];

        val /= (float)num_top_k;

        for (int k = 0; k < num_top_k; k++)
            mes_pixel[p][i][k] -= val;
    }
}

Vec2f WTA_PerPixel_Min_PMF_PMBP(float* pCost2, Mat_<Vec2f>& plabel, int p, int num_top_k)
{
    float Min_tp = pCost2[0];
    int Disp = 0;
    for (int k = 0; k < num_top_k; k++) {
        if (pCost2[k] < Min_tp) {
            Min_tp = pCost2[k];
            Disp = k;
        }
    }
    return plabel[p][Disp];
}

void spm_bp::preProcessing()
{
    // Superpixels AND Build Graph
    createAndOrganizeSuperpixels();
    BuildSuperpixelsPropagationGraph(segLabels1, numOfSP1, im1f, spGraph1[0], spGraph1[1]);
    AssociateLeftImageItselfToEstablishNonlocalPropagation(30, 5);

    // Random Assign Representative Pixel
    RandomAssignRepresentativePixel(superpixelsList1, numOfSP1, repPixels1);

    // Initiate Filter
    initiateData();
}
void spm_bp::runspm_bp(cv::Mat_<cv::Vec2f>& flowResult)
{
    clock_t start_disp, finish_disp;
    start_disp = clock();
    printf("==================================================\n");
    printf("SPM-BP begins\n");

    label_k.create(height1 * width1, NUM_TOP_K);
    dcost_k.create(height1 * width1, NUM_TOP_K);

    //initiate labels
    init_label_super(label_k, dcost_k);

    //MESSAGE 0:left, 1:right, 2:up, 3:down
    Mat_<Vec<float, NUM_TOP_K> > message(height1 * width1, 4);
    message.setTo(0);

    //precompute smooth weight
    float omega[256];
    for (int i = 0; i < 256; ++i)
        omega[i] = lambda_smooth * std::exp(-float(i) / 20);

    // TODO: Change to Mat4f
    Mat_<Vec4f> smoothWt(height1, width1);
    smoothWt.setTo(lambda_smooth);

    for (int i = 1; i < height1 - 1; ++i)
    {
        for (int j = 1; j < width1 - 1; ++j) {
            const Vec3f &ref = im1f[i][j];
            // TODO: Don't need abs here since norm >= 0
            smoothWt[i][j][0] = omega[int(norm(ref - im1f[i][j - 1]))];
            smoothWt[i][j][1] = omega[int(norm(ref - im1f[i][j + 1]))];
            smoothWt[i][j][2] = omega[int(norm(ref - im1f[i - 1][j]))];
            smoothWt[i][j][3] = omega[int(norm(ref - im1f[i + 1][j]))];
        }
    }

    float dis_belief_l[NUM_TOP_K];
    float dis_belief_r[NUM_TOP_K];
    float dis_belief_u[NUM_TOP_K];
    float dis_belief_d[NUM_TOP_K];

    const int BUFSIZE = NUM_TOP_K * 50;

    vector<Vec2f> vec_label(BUFSIZE), vec_label_nei(BUFSIZE);
    vector<float> vec_mes_l(BUFSIZE),
                  vec_mes_r(BUFSIZE),
                  vec_mes_u(BUFSIZE),
                  vec_mes_d(BUFSIZE),
                  vec_belief(BUFSIZE),
                  vec_d_cost(BUFSIZE);
    Mat_<float> DataCost_nei;

    start_disp = clock();
    double wall_timer = omp_get_wtime();
    if (display)
        Show_WTA_Flow(-1, label_k, dcost_k, message, flowResult);

    int spBegin, spEnd, spStep;
    for (int iter = 0; iter < iterNum; iter++) {
        if (iter % 2) {
            spBegin = numOfSP1 - 1, spEnd = -1, spStep = -1;
        }
        else {
            spBegin = 0, spEnd = numOfSP1, spStep = 1;
        }

        for (int sp = spBegin; sp != spEnd; sp += spStep) {
            int curSPP = superpixelsList1[sp][0];
            Vec4i curRange = subRange1[curSPP];
            int y = curRange[0];
            int x = curRange[1];
            int w = curRange[2] - curRange[0] + 1;
            int h = curRange[3] - curRange[1] + 1;

            // rand neighbor pixel and store
            vec_label_nei.clear();
            std::set<int>::iterator sIt;
            std::set<int>& sAdj = spGraph1[iter % 2].adjList[sp];
            for (sIt = sAdj.begin(); sIt != sAdj.end(); ++sIt) {
                repPixels1[*sIt] = superpixelsList1[*sIt][rand() % superpixelsList1[*sIt].size()];
                Vec2f test_label;
                for (int k = 0; k < NUM_TOP_K; k++) {
                    test_label = label_k[repPixels1[*sIt]][k];
                    if (isNewLabel_PMF_PMBP(vec_label_nei, test_label) == -1)
                        vec_label_nei.push_back(test_label);
                }
            }

            //repPixels1[sp] = superpixelsList1[sp][rand() % superpixelsList1[sp].size()];
            //for (int k = 0; k < NUM_TOP_K; k++) {
            //    Vec2f test_label = label_k[repPixels1[sp]][k];
            //    float mag = min(disp_range_u, disp_range_v) / 8;
            //    for (; mag >= (1.0 / upScale); mag /= 2.0) {
            //        Vec2f test_label_random;
            //        float tmpVerLabel = test_label[0] + ((float(rand()) / RAND_MAX) - 0.5) * 2.0 * mag;
            //        float tmpHorLabel = test_label[1] + ((float(rand()) / RAND_MAX) - 0.5) * 2.0 * mag;

            //        test_label_random[0] = 0;
            //        test_label_random[1] = floor(tmpHorLabel * upScale + 0.5) / upScale;

            //        if (test_label_random[0] >= -disp_range_u && test_label_random[0] <= disp_range_u
            //            && test_label_random[1] >= -disp_range_v && test_label_random[1] <= disp_range_v
            //            && isNewLabel_PMF_PMBP(vec_label_nei, test_label_random) == -1) //here
            //        {
            //            vec_label_nei.push_back(test_label_random);
            //        }
            //    }
            //}
            const int vec_size = vec_label_nei.size();

            DataCost_nei.create(h, w * vec_size);
            DataCost_nei.setTo(0);
#if USE_CLMF0_TO_AGGREGATE_COST
            cv::Mat_<cv::Vec4b> leftCombinedCrossMap;
            leftCombinedCrossMap.create(h, w);
            subCrossMap1[sp].copyTo(leftCombinedCrossMap);
            CFFilter cff;
#endif

#pragma omp parallel for num_threads(NTHREADS)
            for (int i = 0; i < vec_size; ++i) {
                int kx = i * w;
                Mat_<float> rawCost;
                getLocalDataCostPerlabel(sp, vec_label_nei[i], rawCost);
#if USE_CLMF0_TO_AGGREGATE_COST
                cff.FastCLMF0FloatFilterPointer(leftCombinedCrossMap, rawCost, rawCost);
#endif
                rawCost.copyTo(DataCost_nei(cv::Rect(kx, 0, w, h)));
            }

            Vec4i curRange_s = spRange1[curSPP];

            int spy_s, spy_e, spx_s, spx_e, spx_step, spy_step;
            spy_s = curRange_s[0];
            spy_e = curRange_s[2] + 1;
            spx_s = curRange_s[1];
            spx_e = curRange_s[3] + 1;
            spx_step = 1;
            spy_step = 1;

            if (iter % 4 == 1) {
                spy_s = curRange_s[2];
                spy_e = curRange_s[0] - 1;
                spx_s = curRange_s[3];
                spx_e = curRange_s[1] - 1;
                spx_step = -1;
                spy_step = -1;
            }
            else if (iter % 4 == 2) {
                spy_s = curRange_s[2];
                spy_e = curRange_s[0] - 1;
                spy_step = -1;
                spx_s = curRange_s[1];
                spx_e = curRange_s[3] + 1;
                spx_step = 1;
            }
            else if (iter % 4 == 3) 
            {
                spy_s = curRange_s[0];
                spy_e = curRange_s[2] + 1;
                spy_step = 1;
                spx_s = curRange_s[3];
                spx_e = curRange_s[1] - 1;
                spx_step = -1;
            }

            for (int bi = spx_s; bi != spx_e; bi = bi + spx_step)
                for (int bj = spy_s; bj != spy_e; bj = bj + spy_step) {

                    int p1 = bi * width1 + bj;
                    int pl = bi * width1 + (bj - 1);
                    int pu = (bi - 1) * width1 + bj;
                    int pr = bi * width1 + (bj + 1);
                    int pd = (bi + 1) * width1 + bj;

                    //Compute disbelief: three incoming message + data_cost
                    for (int k = 0; k < NUM_TOP_K; ++k) {
                        if (bj != 0)
                            dis_belief_l[k] = message[pl][0][k] + message[pl][2][k] + message[pl][3][k] + dcost_k[pl][k];
                        if (bj != width1 - 1)
                            dis_belief_r[k] = message[pr][1][k] + message[pr][2][k] + message[pr][3][k] + dcost_k[pr][k];
                        if (bi != 0)
                            dis_belief_u[k] = message[pu][0][k] + message[pu][1][k] + message[pu][2][k] + dcost_k[pu][k];
                        if (bi != height1 - 1)
                            dis_belief_d[k] = message[pd][0][k] + message[pd][1][k] + message[pd][3][k] + dcost_k[pd][k];
                    }

                    vec_label.clear();
                    vec_mes_l.clear();
                    vec_mes_r.clear();
                    vec_mes_u.clear();
                    vec_mes_d.clear();
                    vec_belief.clear();
                    vec_d_cost.clear();

                    // Update and save messages with current reference pixel's labels
                    Vec4f wt_s = smoothWt[bi][bj];

                    for (int k = 0; k < NUM_TOP_K; ++k) {
                        Vec2f test_label = label_k[p1][k];
                        vec_label.push_back(test_label);
                        float dcost = dcost_k[p1][k];
                        vec_d_cost.push_back(dcost);
                        //start_disp = clock();
                        float _mes_l = 0, _mes_r = 0, _mes_u = 0, _mes_d = 0;
                        if (bj != 0) {
                            _mes_l = ComputeMes_PMBP_per_label(dis_belief_l, label_k, pl, test_label, wt_s[0], tau_s);
                            vec_mes_l.push_back(_mes_l);
                        }
                        if (bj != width1 - 1) {
                            _mes_r = ComputeMes_PMBP_per_label(dis_belief_r, label_k, pr, test_label, wt_s[1], tau_s);
                            vec_mes_r.push_back(_mes_r);
                        }
                        if (bi != 0) {
                            _mes_u = ComputeMes_PMBP_per_label(dis_belief_u, label_k, pu, test_label, wt_s[2], tau_s);
                            vec_mes_u.push_back(_mes_u);
                        }
                        if (bi != height1 - 1) {
                            _mes_d = ComputeMes_PMBP_per_label(dis_belief_d, label_k, pd, test_label, wt_s[3], tau_s);
                            vec_mes_d.push_back(_mes_d);
                        }
                        vec_belief.push_back(_mes_l + _mes_r + _mes_u + _mes_d + dcost);
                    }

                    //propagation + random search
                    int kx;
                    for (int test_id = 0; test_id < vec_label_nei.size(); ++test_id) {
                        Vec2f test_label = vec_label_nei[test_id];
                        //if(isNewLabel_PMF_PMBP(vec_label,test_label)==-1)
                        {
                            vec_label.push_back(test_label);
                            kx = test_id * w;
                            const Mat_<float>& local = DataCost_nei(cv::Rect(kx, 0, w, h));
                            float dcost = local[bi - x][bj - y];
                            vec_d_cost.push_back(dcost);
                            //start_disp = clock();
                            float _mes_l = 0, _mes_r = 0, _mes_u = 0, _mes_d = 0;
                            if (bj != 0) {
                                _mes_l = ComputeMes_PMBP_per_label(dis_belief_l, label_k, pl, test_label, wt_s[0], tau_s);
                                vec_mes_l.push_back(_mes_l);
                            }
                            if (bj != width1 - 1) {
                                _mes_r = ComputeMes_PMBP_per_label(dis_belief_r, label_k, pr, test_label, wt_s[1], tau_s);
                                vec_mes_r.push_back(_mes_r);
                            }
                            if (bi != 0) {
                                _mes_u = ComputeMes_PMBP_per_label(dis_belief_u, label_k, pu, test_label, wt_s[2], tau_s);
                                vec_mes_u.push_back(_mes_u);
                            }
                            if (bi != height1 - 1) {
                                _mes_d = ComputeMes_PMBP_per_label(dis_belief_d, label_k, pd, test_label, wt_s[3], tau_s);
                                vec_mes_d.push_back(_mes_d);
                            }
                            vec_belief.push_back(_mes_l + _mes_r + _mes_u + _mes_d + dcost);
                        }
                    }

                    Compute_top_k(vec_belief, vec_label, vec_mes_l, vec_mes_r, vec_mes_u, vec_mes_d, vec_d_cost, message, label_k, dcost_k, p1, NUM_TOP_K);
                    //cout<<label_k[p1][0]<<endl;
                    Message_normalization_PMF_PMBP(message, p1, NUM_TOP_K);
                }
            //cout<<"Message "<<clock()-start_disp<<endl;
            // propagation end
        } //superpixel scan end
        if (display)
            Show_WTA_Flow(iter, label_k, dcost_k, message, flowResult);
        finish_disp = clock();
        std::cout << " time on clock(): " << (double)(clock() - start_disp) / CLOCKS_PER_SEC
                  << " time on wall: " << omp_get_wtime() - wall_timer << "\n";
    } //iteration
    Show_WTA_Flow(iterNum, label_k, dcost_k, message, flowResult);
    printf("==================================================\n");
    printf("SPM-BP finished\n==================================================\n");
}

void BuildCensus_bitset(const Mat_<float> &imgGray,
                        int winSize,
                        vector<vector<bitset<CENSUS_SIZE_OF> > >& CensusStr,
                        int ImgHeight, int ImgWidth, int gap)
{
    int ix0, iy0, iyy;
    int x, y;
    int PixIdx;
    int censusIdx;
    float centerValue, tempValue;
    int hei_side = (winSize - 1) * gap / 2;
    int wid_side = (winSize - 1) * gap / 2;
    CensusStr.clear();
    CensusStr.resize(ImgHeight);
    //#pragma omp parallel for private(ix0, iy0, x, y, PixIdx, censusIdx, centerValue, tempValue) num_threads(NTHREADS)
    for (iy0 = 0; iy0 < ImgHeight; ++iy0) {
        CensusStr[iy0].clear();
        CensusStr[iy0].resize(ImgWidth);
        for (ix0 = 0; ix0 < ImgWidth; ++ix0) {
            centerValue = imgGray[iy0][ix0];
            censusIdx = 0;

            for (y = -hei_side; y <= hei_side; y = y + gap) {
                for (x = -wid_side; x <= wid_side; x = x + gap) {
                    if (iy0 + y < 0 || iy0 + y >= ImgHeight || ix0 + x < 0 || ix0 + x >= ImgWidth) {
                        tempValue = centerValue;
                    }
                    else {
                        tempValue = imgGray[iy0 + y][ix0 + x];
                    }

                    CensusStr[iy0][ix0][censusIdx] = centerValue > tempValue ? 1 : 0;
                    ++censusIdx;
                }
            }
        }
    }
}

void spm_bp::initiateData()
{
    //// upsample image
    printf("==================================================\n");
    printf("Preparing...");
    width1_up = width1 * upScale;
    height1_up = height1 * upScale;
    clock_t start = clock();
    double wall_timer = omp_get_wtime();
    // left
    cv::resize(im1f, im1Up, cv::Size(width1 * upScale, height1 * upScale), 0.0, 0.0, CV_INTER_CUBIC);
    cv::Mat_<float> tmp1GrayUp;
    cv::cvtColor(im1Up, tmp1GrayUp, CV_BGR2GRAY);
    BuildCensus_bitset(tmp1GrayUp, CENSUS_WINSIZE, censusBS1, height1_up, width1_up, upScale);
    tmp1GrayUp.release();
    // right
    cv::resize(im2f, im2Up, cv::Size(width2 * upScale, height2 * upScale), 0.0, 0.0, CV_INTER_CUBIC);
    cv::Mat_<float> tmp2GrayUp;
    cv::cvtColor(im2Up, tmp2GrayUp, CV_BGR2GRAY);
    BuildCensus_bitset(tmp2GrayUp, CENSUS_WINSIZE, censusBS2, height1_up, width1_up, upScale);
    tmp2GrayUp.release();

    //// building sensus
    float miu = 0.386 * CENSUS_SIZE_OF;
    for (int iy = 0; iy <= CENSUS_SIZE_OF; ++iy)
        expCensusDiffTable[iy] = 1.0 - exp(-(double)iy / 30);

    for (int iy = 0; iy < 256; ++iy)
        expColorDiffTable[iy] = 1.0 - exp(-(double)iy / 60);

    int iy;
#if USE_CENSUS
    subCensusBS1.clear();
    subCensusBS1.resize(numOfSP1);
    subCensusBS2.clear();
    subCensusBS2.resize(numOfSP2);
#endif
    if (DO_LEFT) {
        subImage1.clear();
        subImage1.resize(numOfSP1);

        for (iy = 0; iy < numOfSP1; ++iy) {
            int id = repPixels1[iy];
            // extract sub-image from subrange
            int w = subRange1[id][2] - subRange1[id][0] + 1;
            int h = subRange1[id][3] - subRange1[id][1] + 1;
            int x = subRange1[id][0];
            int y = subRange1[id][1];

            subImage1[iy] = im1f(cv::Rect(x, y, w, h)).clone();

#if USE_CENSUS
            int ky, kx;
            subCensusBS1[iy].resize(h);
            for (ky = 0; ky < h; ++ky) {
                subCensusBS1[iy][ky].resize(w);
                for (kx = 0; kx < w; ++kx) {
                    subCensusBS1[iy][ky][kx] = censusBS1[(y + ky) * upScale][(x + kx) * upScale];
                }
            }
#endif
        }
    }

#if USE_CLMF0_TO_AGGREGATE_COST
    // initiate cross-map of image
    // various filter options
    crossColorTau = g_filterKernelColorTau;
    crossArmLength = g_filterKernelSize;

    // calculate sub-image and sub-crossmap
    cv::Mat im1Blur, im2Blur;
    cv::medianBlur(im1d, im1Blur, 3);
    cv::medianBlur(im2d, im2Blur, 3);
    CFFilter cf;
    if (DO_LEFT) {
        cf.GetCrossUsingSlidingWindow(im1Blur, crossMap1, crossArmLength, crossColorTau);
        subCrossMap1.clear();
        subCrossMap1.resize(numOfSP1);

        for (iy = 0; iy < numOfSP1; ++iy) {
            int id = repPixels1[iy];
            // extract sub-image from subrange
            int w = subRange1[id][2] - subRange1[id][0] + 1;
            int h = subRange1[id][3] - subRange1[id][1] + 1;
            int x = subRange1[id][0];
            int y = subRange1[id][1];

            cv::Mat_<cv::Vec4b> tmpCr;
            ModifyCrossMapArmlengthToFitSubImage(crossMap1(cv::Rect(x, y, w, h)), crossArmLength, tmpCr);
            subCrossMap1[iy] = tmpCr.clone();
        }
    }
#endif
    std::cout << " time on clock(): " << (double)(clock() - start) / CLOCKS_PER_SEC
              << " time on wall: " << omp_get_wtime() - wall_timer;
    printf("Done!\n");
    printf("==================================================\n");
}

void spm_bp::init_label_super(Mat_<Vec2f>& label_super_k, Mat_<float>& dCost_super_k) //, vector<vector<Vec2f> > &label_saved, vector<vector<Mat_<float> > > &dcost_saved)
{
    printf("==================================================\n");
    printf("Initiating particles...Done!\n");
    vector<Vec2f> label_vec;
    Mat_<float> localDataCost;
    for (int sp = 0; sp < numOfSP1; ++sp) {
        int id = repPixels1[sp];
        int y = subRange1[id][0];
        int x = subRange1[id][1];
        int h = subRange1[id][3] - subRange1[id][1] + 1;
        int w = subRange1[id][2] - subRange1[id][0] + 1;

        label_vec.clear();
        int k = 0;

        while (k < NUM_TOP_K) {
            float dv = (float(rand()) / RAND_MAX - 0.5) * 2 * (float)disp_range_v;
			float du = (float(rand()) / RAND_MAX - 0.5) * 2 * (float)disp_range_u;;
            dv = abs(floor(dv * upScale + 0.5) / upScale) * positive;
			du = floor(du * upScale + 0.5) / upScale;
            //dv = 0;

            if (du >= -disp_range_u && du <= disp_range_u && dv >= -disp_range_v && dv <= disp_range_v) {
                int index_tp = 1;
                for (int k1 = 0; k1 < k; ++k1) {
                    if (checkEqual_PMF_PMBP(label_super_k[repPixels1[sp]][k1], Vec2f(du, dv)))
                        index_tp = 0;
                }

                if (index_tp == 1) {
                    for (int ii = 0; ii < superpixelsList1[sp].size(); ++ii)
                        label_super_k[superpixelsList1[sp][ii]][k] = Vec2f(du, dv);

                    label_vec.push_back(Vec2f(du, dv));
                    ++k;
                }
            }
        }
#if USE_CLMF0_TO_AGGREGATE_COST
        cv::Mat_<cv::Vec4b> leftCombinedCrossMap;
        leftCombinedCrossMap.create(h, w);
        subCrossMap1[sp].copyTo(leftCombinedCrossMap);
        CFFilter cff;
#endif
        int vec_size = label_vec.size();
        localDataCost.create(h, w * vec_size);
        localDataCost.setTo(0);
#pragma omp parallel for num_threads(NTHREADS)
        for (int i = 0; i < vec_size; ++i) {
            int kx = i * w;
            Mat_<float> rawCost;
            getLocalDataCostPerlabel(sp, label_vec[i], rawCost);
#if USE_CLMF0_TO_AGGREGATE_COST
            cff.FastCLMF0FloatFilterPointer(leftCombinedCrossMap, rawCost, rawCost);
#endif
            rawCost.copyTo(localDataCost(cv::Rect(kx, 0, w, h)));
        }

        //getLocalDataCost( sp, label_vec, localDataCost);

        int pt, px, py, kx;
        for (int ii = 0; ii < superpixelsList1[sp].size(); ++ii) {
            //cout<<ii<<endl;
            pt = superpixelsList1[sp][ii];
            px = pt / width1;
            py = pt % width1;
            for (int kk = 0; kk < NUM_TOP_K; kk++) {
                kx = kk * w;
                const Mat_<float>& local = localDataCost(cv::Rect(kx, 0, w, h));
                dCost_super_k[pt][kk] = local[px - x][py - y];
            }
        }
    }
    printf("==================================================\n");
}

void spm_bp::getLocalDataCostPerlabel(int sp, const Vec2f& fl, Mat_<float>& localDataCost)
{
    //USE_COLOR_FEATURES
    cv::Mat_<cv::Vec3f> subLt = subImage1[sp];
#if USE_CENSUS
    vector<vector<bitset<CENSUS_SIZE_OF> > > subLt_css = subCensusBS1[sp];
#endif

    int upHeight, upWidth;
    upHeight = im1Up.rows;
    upWidth = im1Up.cols;

    // extract sub-image from subrange
    int p1 = repPixels1[sp];
    int w = subRange1[p1][2] - subRange1[p1][0] + 1;
    int h = subRange1[p1][3] - subRange1[p1][1] + 1;
    int x = subRange1[p1][0];
    int y = subRange1[p1][1];

    // localDataCost.release();
    localDataCost.create(h, w);
    //Mat_<float> rawCost(h, w);
    Mat_<Vec3f> subRt(h, w);
    vector<vector<bitset<CENSUS_SIZE_OF> > > subRt_css(h, vector<bitset<CENSUS_SIZE_OF> >(w));
#if SAVE_DCOST
    if (check_id >= 0) {
        cout << "hit" << endl;
    }
#endif

    Vec3f* subLtPtr = (cv::Vec3f*)(subLt.ptr(0));
    Vec3f* subRtPtr = (cv::Vec3f*)(subRt.ptr(0));
    float* rawCostPtr = (float*)(localDataCost.ptr(0));

    cv::Vec3f *im2UpPtr = (cv::Vec3f*) im2Up.ptr(0);
    int im2UpWidth = im2Up.cols;
    int maxHeight = upHeight - 1;
    int maxWidth = upWidth - 1;

    int cy, cx, oy, ox;
    float fl0 = fl[0], fl1 = fl[1];
#pragma omp parallel for
    for (cy = 0; cy < h; ++cy) {
        oy = y + cy;
        int oyUp = (oy + fl0) * upScale;
        if (oyUp < 0)
            oyUp = 0;
        if (oyUp >= upHeight)
            oyUp = maxHeight;

        for (cx = 0; cx < w; ++cx) {
            ox = x + cx;
            int oxUp = (ox + fl1) * upScale;
            if (oxUp < 0)
                oxUp = 0;
            if (oxUp >= upWidth)
                oxUp = maxWidth;

#if USE_POINTER_WISE
            // *subRtPtr++ = im2Up[oyUp][oxUp];
            subRtPtr[cy * w + cx] = im2UpPtr[oyUp * im2UpWidth + oxUp];
#else
            subRt[cy][cx] = im2Up[oyUp][oxUp];
#endif

#if USE_CENSUS
            subRt_css[cy][cx] = censusBS2[oyUp][oxUp];
#endif
        }

    }

    // calculate raw cost
    subLtPtr = (cv::Vec3f*)(subLt.ptr(0));
    subRtPtr = (cv::Vec3f*)(subRt.ptr(0));
    rawCostPtr = (float*)(localDataCost.ptr(0));

    int iy, ix;
    for (iy = 0; iy < h; ++iy) {
        for (ix = 0; ix < w; ++ix) {

#if DATA_COST_ADCENSUS
            bitset<CENSUS_SIZE_OF> tmpBS = subRt_css[iy][ix] ^ subLt_css[iy][ix];

#if USE_POINTER_WISE

            float dist_c = fabsf((*subLtPtr)[0] - (*subRtPtr)[0])
                         + fabsf((*subLtPtr)[1] - (*subRtPtr)[1])
                         + fabsf((*subLtPtr)[2] - (*subRtPtr)[2]);
            ++subLtPtr;
            ++subRtPtr;
#else
            float dist_c = std::abs(subLt[iy][ix][0] - subRt[iy][ix][0])
                + std::abs(subLt[iy][ix][1] - subRt[iy][ix][1])
                + std::abs(subLt[iy][ix][2] - subRt[iy][ix][2]);
#endif

            float dist_css = expCensusDiffTable[tmpBS.count()];
            float dist_ce = expColorDiffTable[int(dist_c / 3)];

#if USE_POINTER_WISE
            *rawCostPtr++ = 255 * (dist_css + dist_ce);
#else
            localDataCost[iy][ix] = 255 * (dist_css + dist_ce); //beta*min(dist_c/3,tau_c);//beta*min(dist_c/3,tau_c);//*255 + beta*min(dist_c/3,tau_c);
#endif

#endif
        }
    }

#if SAVE_DCOST
    label_saved[sp].push_back(fl);
#endif

}

int spm_bp::createAndOrganizeSuperpixels()
{
    Mat img1 = im1d;
    Mat img2 = im2d;

    Mat_<int> label1, label2;
    int numLabel1, numLabel2;
    vector<Vec4i> sub1, sub2;
    vector<Vec4i> sp1, sp2;

#pragma omp parallel for
    for (int i = 0; i < 2; ++i) {
        if (i == 0) {
            numOfSP1 = CreateSLICSegments(img1, label1, g_spNumber, g_spSize, g_spSizeOrNumber);
            GetSubImageRangeFromSegments(label1, numOfSP1, g_filterKernelBoundarySize, sub1, sp1);
            cout << "sp1" << endl;
        }
        else {
            numOfSP2 = CreateSLICSegments(img2, label2, g_spNumber, g_spSize, g_spSizeOrNumber);
            GetSubImageRangeFromSegments(label2, numOfSP2, g_filterKernelBoundarySize, sub2, sp2);
            cout << "sp2" << endl;
        }
    }

    printf("==================================================\n");
    printf("Created [%d, %d] segments and sub-images\n", numOfSP1, numOfSP2);

    subRange1 = sub1;
    spRange1 = sp1;
    segLabels1 = label1.clone();

    subRange2 = sub2;
    spRange2 = sp2;
    segLabels2 = label2.clone();

    GetSuperpixelsListFromSegment(segLabels1, numOfSP1, superpixelsList1);
    GetSuperpixelsListFromSegment(segLabels2, numOfSP2, superpixelsList2);
    printf("==================================================\n");

    return 0;
}

void spm_bp::GetSuperpixelsListFromSegment(const Mat_<int>& segLabels, int numOfLabels, vector<vector<int> >& spPixelsList)
{
    int iy, ix, height, width;
    //height = segLabels.rows;
    //width = segLabels.cols;

    spPixelsList.clear();
    spPixelsList.resize(numOfLabels);
    for (iy = 0; iy < numOfLabels; ++iy)
        spPixelsList[iy].clear();
    for (iy = 0; iy < height1; ++iy) {
        for (ix = 0; ix < width1; ++ix) {
            int tmpLabel = segLabels[iy][ix];
            spPixelsList[tmpLabel].push_back(iy * width1 + ix);
        }
    }
}

void spm_bp::ModifyCrossMapArmlengthToFitSubImage(const cv::Mat_<cv::Vec4b>& crMapIn, int maxArmLength, cv::Mat_<cv::Vec4b>& crMapOut)
{
    int iy, ix, height, width;
    height = crMapIn.rows;
    width = crMapIn.cols;
    crMapOut = crMapIn.clone();
    // up
    for (iy = 0; iy < min<int>(maxArmLength, height); ++iy) {
        for (ix = 0; ix < width; ++ix) {
            crMapOut[iy][ix][1] = min<int>(iy, crMapOut[iy][ix][1]);
        }
    }

    // down
    int ky = maxArmLength - 1;
    for (iy = height - maxArmLength; iy < height; ++iy) {
        if (iy < 0) {
            --ky;
            continue;
        }
        for (ix = 0; ix < width; ++ix) {
            crMapOut[iy][ix][3] = min<int>(ky, crMapOut[iy][ix][3]);
        }
        --ky;
    }

    // left
    for (iy = 0; iy < height; ++iy) {
        for (ix = 0; ix < min<int>(width, maxArmLength); ++ix) {
            crMapOut[iy][ix][0] = min<int>(ix, crMapOut[iy][ix][0]);
        }
    }

    // right
    int kx;
    for (iy = 0; iy < height; ++iy) {
        kx = maxArmLength - 1;
        for (ix = width - maxArmLength; ix < width; ++ix) {
            if (ix < 0) {
                --kx;
                continue;
            }
            crMapOut[iy][ix][2] = min<int>(kx, crMapOut[iy][ix][2]);
            --kx;
        }
    }
}

void spm_bp::RandomAssignRepresentativePixel(const vector<vector<int> >& spPixelsList, int numOfLabels, vector<int>& rePixel)
{
    rePixel.resize(numOfLabels);
    RNG rng;
    int iy;
    for (iy = 0; iy < numOfLabels; ++iy) {
        rePixel[iy] = spPixelsList[iy][rng.next() % spPixelsList[iy].size()];
    }
}

void spm_bp::BuildSuperpixelsPropagationGraph(const cv::Mat_<int>& refSegLabel, int numOfLabels, const cv::Mat_<cv::Vec3f>& refImg, GraphStructure& spGraphEven, GraphStructure& spGraphOdd)
{
    printf("==================================================\n");
    printf("Buiding Superpixel Graph...");
    spGraphEven.adjList.clear();
    spGraphEven.vertexNum = 0;
    // build superpixel connectivity graph
    spGraphEven.ReserveSpace(numOfLabels * 20);
    spGraphEven.SetVertexNum(numOfLabels);
    spGraphOdd.adjList.clear();
    spGraphOdd.vertexNum = 0;
    // build superpixel connectivity graph
    spGraphOdd.ReserveSpace(numOfLabels * 20);
    spGraphOdd.SetVertexNum(numOfLabels);
    int iy, ix, height, width;
    height = refSegLabel.rows;
    width = refSegLabel.cols;
    for (iy = 0; iy < height; ++iy) {
        for (ix = 0; ix < width; ++ix) {
            int tmp1 = refSegLabel[iy][ix];
            if (iy > 0) {
                int tmp2 = refSegLabel[iy - 1][ix];
                if (tmp1 != tmp2) {
                    spGraphEven.AddEdge(tmp1, tmp2);
                    spGraphOdd.AddEdge(tmp2, tmp1);
                }
            }

            if (ix > 0) {
                int tmp2 = refSegLabel[iy][ix - 1];
                if (tmp1 != tmp2) {
                    spGraphEven.AddEdge(tmp1, tmp2);
                    spGraphOdd.AddEdge(tmp2, tmp1);
                }
            }
            /*
			if (iy < height-1) 
			{
				int tmp2 = refSegLabel[iy+1][ix];
				if (tmp1 != tmp2)
				{
					spGraphOdd.AddEdge(tmp1, tmp2);
					//spGraph.AddEdge(tmp2, tmp1);
				}
			}

			if (ix < width-1)
			{
				int tmp2 = refSegLabel[iy][ix+1];
				if (tmp1 != tmp2)
				{
					spGraphOdd.AddEdge(tmp1, tmp2);
					//spGraph.AddEdge(tmp2, tmp1);
				}
			}*/
        }
    }
    printf("Done!\n");
    printf("==================================================\n");
}

struct CANDIDATE_TYPE {
    float sumWeight;
    int spId;
    CANDIDATE_TYPE(float sw, int si)
        : sumWeight(sw)
        , spId(si)
    {
    }
};

bool COMP_CANDIDATE_TYPE(const CANDIDATE_TYPE& ct1, const CANDIDATE_TYPE& ct2)
{
    return (ct1.sumWeight > ct2.sumWeight);
}

void spm_bp::AssociateLeftImageItselfToEstablishNonlocalPropagation(int sampleNum, int topK)
{
    //cout<<width1<<" "<<width2<<endl;
    const float MAX_FL = std::max<float>(disp_range_u, disp_range_u);
    const float sigmaSpatial = MAX_FL * MAX_FL;
    const float sigmaColor = 25.0 * 25.0;
    const float MAX_SPATIAL_DISTANCE = MAX_FL * MAX_FL * 6.25;
    int iLt, iRt;
    for (iLt = 0; iLt < numOfSP1; ++iLt) {
        vector<CANDIDATE_TYPE> vecCandi;
        vecCandi.clear();
        int ltSpSize = superpixelsList1[iLt].size();
        Vec2i pL, pR;
        for (iRt = 0; iRt < numOfSP1; ++iRt) {
            int rtSpSize = superpixelsList1[iRt].size();
            float sumWeight = 0.0;
            int iy;
            //cout<<iLt<<" "<<iRt<<endl;
            for (iy = 0; iy < sampleNum; ++iy) {
                int ptL = superpixelsList1[iLt][rand() % ltSpSize];
                pL[0] = ptL / width1;
                pL[1] = ptL % width1;
                int ptR = superpixelsList1[iRt][rand() % rtSpSize];
                //cout<<ptL<<" "<<ptR<<endl;
                pR[0] = ptR / width1;
                pR[1] = ptR % width1;

                float tmpSpatial = (pL[0] - pR[0]) * (pL[0] - pR[0]) + (pL[1] - pR[1]) * (pL[1] - pR[1]);
                if (tmpSpatial > MAX_SPATIAL_DISTANCE) {
                    break;
                }
                cv::Vec3f pixL, pixR;
                pixL = im1f[pL[0]][pL[1]];
                pixR = im2f[pR[0]][pR[1]];
                float tmpColor = (pixL[0] - pixR[0]) * (pixL[0] - pixR[0])
                    + (pixL[1] - pixR[1]) * (pixL[1] - pixR[1])
                    + (pixL[2] - pixR[2]) * (pixL[2] - pixR[2]);
                float colorDis = exp(-tmpColor / sigmaColor);
                sumWeight += colorDis;
            }

            if (iy >= sampleNum)
                vecCandi.push_back(CANDIDATE_TYPE(sumWeight, iRt));
        }

        std::sort(vecCandi.begin(), vecCandi.end(), COMP_CANDIDATE_TYPE);

        int iy, cnt = 0;
        for (iy = 0; iy < vecCandi.size(); ++iy) {
            if (vecCandi[iy].sumWeight < sampleNum * 0.2)
                break;
            int tmpId = vecCandi[iy].spId;
            // not itself
            if (tmpId != iLt) {
                // not in its spatial adjacency list
                std::set<int>::iterator sIt;
                std::set<int>& sAdj = spGraph1[0].adjList[iLt];
                for (sIt = sAdj.begin(); sIt != sAdj.end(); ++sIt) {
                    if (tmpId == *sIt)
                        break;
                }
                if (sIt == sAdj.end() && tmpId < iLt) {
                    spGraph1[0].AddEdge(iLt, tmpId);
                    if (++cnt > topK)
                        break;
                }
                if (sIt == sAdj.end() && tmpId > iLt) {
                    spGraph1[1].AddEdge(iLt, tmpId);
                    if (++cnt > topK)
                        break;
                }
            }
        }
    }
}

void spm_bp::Show_WTA_Flow(int iter, Mat_<Vec2f>& label_k, Mat_<float>& dCost_k, Mat_<Vec<float, NUM_TOP_K> >& message, cv::Mat_<cv::Vec2f>& flowResult)
{
    float cost_perpixel[NUM_TOP_K];
    Vec2f tmp;
    //Mat_<Vec2f> flow_t(height1,width1);
    for (int i = 0; i < height1; i++)
        for (int j = 0; j < width1; j++) {
            int mm = i * width1 + j;
            for (int k = 0; k < NUM_TOP_K; k++)
                cost_perpixel[k] = dCost_k[mm][k] + message[mm][0][k] + message[mm][1][k] + message[mm][2][k] + message[mm][3][k];
            tmp = WTA_PerPixel_Min_PMF_PMBP(cost_perpixel, label_k, mm, NUM_TOP_K);
            flowResult[i][j][0] = tmp[1];
            flowResult[i][j][1] = tmp[0];
        }

    char fileName[256];

    if (iter >= 0)
        sprintf(fileName, "spmbp_iter=%.3d.png", iter);
    else
        sprintf(fileName, "spmbp_iter= init.png");

    if (display) {
        Mat_<Vec3b> flow_color_t;
        MotionToColor(flowResult, flow_color_t, 30);
        //cv::imwrite(fileName,flow_color_t);
        cv::imshow("Current flow result", flow_color_t);
        cv::waitKey(1);
        flow_color_t.release();
    }
}
