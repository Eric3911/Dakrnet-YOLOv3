#include "yolo_layer.h"
#include "activations.h"
#include "blas.h"
#include "box.h"
#include "dark_cuda.h"
#include "utils.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>


/**************************************************************************************
*  func������yolo�㣬��ʼ��yolo�����ز��������ز�ָ��                               *
*  args batch���ò��batch��������batch��Сһ��                                       *
*  args w����һ�������������������������width                                      *
*  args h: �������������height                                                       *
*  args n: ����anchor�ĸ�����yolo v3��Ϊ3��                                           *
*  args total��anchor���ܸ���                                                         *
*  args mask�����汾��ʹ��anchor������                                                *
*  args classes: �����Ŀ                                                             *
*  args max_boxes�����������                                                     *
**************************************************************************************/
layer make_yolo_layer(int batch, int w, int h, int n, int total, int *mask, int classes, int max_boxes)
{
    int i;
    layer l = { (LAYER_TYPE)0 };
    l.type = YOLO;

    // һ��cell��������Ԥ����ٸ����ο�box��
    l.n = n;
    l.total = total;
    l.batch = batch;
    l.h = h;
    l.w = w;
    // ��yolov3�У�Ԥ��ֵҲ�������ֵ��ÿһά������ÿ�����ĸ���ֵ+4��BOX����Ϣ+1��confidence
    // ÿ��cell��3��anchor������Ϊ����n�������������ά��Ϊ��n*(classes + 4 + 1)
    // yolo_layer������������ߴ�һ�£�ͨ����Ҳһ����Ҳ������һ�㲢���ı��������ݵ�ά��
    l.c = n*(classes + 4 + 1);
    l.out_w = l.w;
    l.out_h = l.h;
    l.out_c = l.c;
    l.classes = classes;
    // cost����Ŀ�꺯��ֵ��Ϊ�����ȸ�����ָ��
    l.cost = (float*)calloc(1, sizeof(float));
    // ƫ�ò�������ʵ�����洢anchor
    l.biases = (float*)calloc(total * 2, sizeof(float));
    if(mask) l.mask = mask;
    // �����ָ��mask ��mask ֵΪ1��n-1
    else{
        l.mask = (int*)calloc(n, sizeof(int));
        for(i = 0; i < n; ++i){
            l.mask[i] = i;
        }
    }
    l.bias_updates = (float*)calloc(n * 2, sizeof(float));
    // һ��ѵ��ͼƬ����yolo_layer���õ������Ԫ�ظ���������������*ÿ������Ԥ��ľ��ο���*ÿ�����ο�Ĳ���������
    l.outputs = h*w*n*(classes + 4 + 1);
    // һ��ѵ��ͼƬ���뵽yolo_layer���Ԫ�ظ�����ע����һ��ͼƬ������yolo_layer������������Ԫ�ظ�����ȣ�
    l.inputs = l.outputs;
    l.max_boxes = max_boxes;
    /*
      ÿ��ͼƬ���е���ʵ���ο�����ĸ�����90��ʾһ��ͼƬ�������90��ground truth���ο�ÿ����ʵ���ο���
      5������������x,y,w,h�ĸ���λ�������Լ��������,ע��90��darknet������д���ģ�ʵ����ÿ��ͼƬ����
      ��û��90����ʵ���ο�Ҳ��û����ô���������Ϊ�˱���һ���ԣ����ǻ�������ô��Ĵ洢�ռ䣬ֻ�����е�
      ֵδ�ն���.
    */
    l.truths = l.max_boxes*(4 + 1);    // 90*(4 + 1);
    l.delta = (float*)calloc(batch * l.outputs, sizeof(float));

    /*
     * yolo_layer�����ά��Ϊl.out_w*l.out_h�����������ά�ȣ����ͨ����Ϊl.out_c����������ͨ������
     * ��ͨ��������n*(classes+4+1)����yolo_layer�����l.output�е��״洢��ʲô�أ��洢��
     * ��������grid cell����Ԥ����ο�box����������Ϣ����Yolo���ľ�֪����Yolo���ģ�����ս�ͼƬ
     * ���ֳ���S*S��������Ϊ13*13��������ÿ��������Ԥ��B����������B=3�����ο����һ������ľ�����Щ
     * ������������������Ԥ����ο���Ϣ��Ŀ����ģ���У������þ��ο�����ʾ����λ��⵽�����壬ÿ�����ο���
     * �����˾��ο�λ��Ϣx,y,w,h��������������Ŷ���Ϣc���Լ����ڸ���ĸ��ʣ������20�࣬��ô���о��ο�
     * ������������������20��ĸ��ʣ���ע���ˣ������ʵ���������е������в�ͬ�����Ȳ�����Ȼ���ܲ�ͬ������
     * ����������������ÿ������Ԥ��3��box��Ҳ�п��ܸ��ࣩ����Ϊ�ؼ����ǣ����ά�ȵļ��㷽ʽ��ͬ���������ᵽ
     * ���һ�������ά��Ϊһ��S_w*S_c*(B*5+C)��tensor����������������S*S��������д��S_w��S_c�ǿ��ǵ�
     * ���񻮷�ά�Ȳ�һ��S_w=S_c=S������ò�������õĶ���S_w=S_c�ģ�����7*7,13*13����֮���׾Ϳ����ˣ���
     * ʵ���ϣ������е㲻ͬ�������ά��Ӧ��ΪS_w*S_c*B*(5+C),CΪ�����Ŀ�����繲��20�ࣻ5����Ϊ��4����λ
     * ��Ϣ�����һ�����Ŷ���Ϣc������5��������Ҳ��ÿ�����ο򶼰���һ�����ڸ���ĸ��ʣ����������о��ο���
     * һ�����ڸ���ĸ��ʣ������Դ�l.outputs�ļ��㷽ʽ�п��������Զ�Ӧ�ϣ�l.out_w = S_w, l.out_c = S_c,
     * l.out_c = B*(5+C)����֪��������״洢ʲô֮�󣬽�����Ҫ��������ô�洢�ģ��Ͼ��������һ����ά������
     * ��ʵ��������һ��һά�������洢�ģ���ϸ��ע�Ϳ��Բο�����forward_yolo_layer()�Լ�entry_index()
     * ������ע�ͣ���������������ֻ��ǱȽ��������ģ�Ӧ�ý���ͼ��˵����
    */
    l.output = (float*)calloc(batch * l.outputs, sizeof(float));
    // Ϊʲô����Ҫ����ôһ����ʼ��anchorֵ�Ĺ�������parse_yolo�в��ǻ�����Ӧ�Ĵ�����
    for(i = 0; i < total*2; ++i){
        l.biases[i] = .5;
    }

    l.forward = forward_yolo_layer;
    l.backward = backward_yolo_layer;
#ifdef GPU
    l.forward_gpu = forward_yolo_layer_gpu;
    l.backward_gpu = backward_yolo_layer_gpu;
    l.output_gpu = cuda_make_array(l.output, batch*l.outputs);
    l.delta_gpu = cuda_make_array(l.delta, batch*l.outputs);

    free(l.output);
    if (cudaSuccess == cudaHostAlloc(&l.output, batch*l.outputs*sizeof(float), cudaHostRegisterMapped)) l.output_pinned = 1;
    else {
        cudaGetLastError(); // reset CUDA-error
        l.output = (float*)calloc(batch * l.outputs, sizeof(float));
    }

    free(l.delta);
    if (cudaSuccess == cudaHostAlloc(&l.delta, batch*l.outputs*sizeof(float), cudaHostRegisterMapped)) l.delta_pinned = 1;
    else {
        cudaGetLastError(); // reset CUDA-error
        l.delta = (float*)calloc(batch * l.outputs, sizeof(float));
    }
#endif

    fprintf(stderr, "yolo\n");
    srand(time(0));

    return l;
}

void resize_yolo_layer(layer *l, int w, int h)
{
    l->w = w;
    l->h = h;

    l->outputs = h*w*l->n*(l->classes + 4 + 1);
    l->inputs = l->outputs;

    if (!l->output_pinned) l->output = (float*)realloc(l->output, l->batch*l->outputs * sizeof(float));
    if (!l->delta_pinned) l->delta = (float*)realloc(l->delta, l->batch*l->outputs*sizeof(float));

#ifdef GPU
    if (l->output_pinned) {
        cudaFreeHost(l->output);
        if (cudaSuccess != cudaHostAlloc(&l->output, l->batch*l->outputs * sizeof(float), cudaHostRegisterMapped)) {
            cudaGetLastError(); // reset CUDA-error
            l->output = (float*)realloc(l->output, l->batch * l->outputs * sizeof(float));
            l->output_pinned = 0;
        }
    }

    if (l->delta_pinned) {
        cudaFreeHost(l->delta);
        if (cudaSuccess != cudaHostAlloc(&l->delta, l->batch*l->outputs * sizeof(float), cudaHostRegisterMapped)) {
            cudaGetLastError(); // reset CUDA-error
            l->delta = (float*)realloc(l->delta, l->batch * l->outputs * sizeof(float));
            l->delta_pinned = 0;
        }
    }

    cuda_free(l->delta_gpu);
    cuda_free(l->output_gpu);

    l->delta_gpu =     cuda_make_array(l->delta, l->batch*l->outputs);
    l->output_gpu =    cuda_make_array(l->output, l->batch*l->outputs);
#endif
}

/************************************************************************************************************
*  func: ��ȡĳ�����ο��4����λ��Ϣ����������ľ��ο�������l.output�л�ȡ�þ��ο�Ķ�λ��Ϣx,y,w,h��.      *
*  args x��yolo_layer���������l.output����������batchԤ��õ��ľ��ο���Ϣ                                  *
*  args biases: �������ǧ��������󵼣������ŵ������е�anchor,yolo3�Ļ���2*9=18��ֵ                    *
*  args n: ����ÿ��yolo����anchor�������������һ��yolo���������mask=0��1��2 �˴���n�ͱ�ʾ���0,1,2������  *
*  args index: ���ο���׵�ַ�����������ο��д洢���׸�����x��l.output�е�������                            *
*  args i: ������                                                                                           *
*  args j: ������                                                                                           *
*  args lw: �ò��width                                                                                     *
*  args lh: �ò��height                                                                                    *
*  args w: �����width                                                                                      *
*  args h: �����height                                                                                     *
*  args stride: �������ʵ����Ҫ����˼��ÿ��ͼƬ�ܹ��ж��ٸ�cell? ֵΪ��l.w*l.h                             *
************************************************************************************************************/
box get_yolo_box(float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, int stride)
{
    box b;
    // ln - natural logarithm (base = e)
    // x` = t.x * lw - i;   // x = ln(x`/(1-x`))   // x - output of previous conv-layer
    // y` = t.y * lh - i;   // y = ln(y`/(1-y`))   // y - output of previous conv-layer
                            // w = ln(t.w * net.w / anchors_w); // w - output of previous conv-layer
                            // h = ln(t.h * net.h / anchors_h); // h - output of previous conv-layer
    // x[index+0*stride]ΪԤ���x��ֵ������i����ΪԤ��ֵ�������cell���������еģ����磬��ǰcellΪ�����У������е�cell
    // ���ʱi=3,j=2����x����3��ʾ����x������ͼ�ϵľ���ֵ������������ͼ�Ŀ�ȼ��õ������ֵ
    b.x = (i + x[index + 0*stride]) / lw;
    // �˴��൱��֪����X��index,Ҫ��Y��index,���ƫ��l.w*l.h������
    b.y = (j + x[index + 1*stride]) / lh;
    // �˴�exp����������֤w��hΪɶ����logis�������
    // ����x[index + 2*stride]ȡ����Ӧ��Wֵ������exp��������biases[2*n]�õ��������Ӧanchor��w������������Ŀ�ȵõ������������ı���
    // �˴�����b.wҪ������ĵĹ�ʽ��⣬�ο� https://blog.csdn.net/hrsstudy/article/details/70305791 ���������
    b.w = exp(x[index + 2*stride]) * biases[2*n]   / w;
    b.h = exp(x[index + 3*stride]) * biases[2*n+1] / h;
    return b;
}

// ���ܣ�box delta����ûʲô��˵�ģ�����square error����
float delta_yolo_box(box truth, float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, float *delta, float scale, int stride)
{
    // �õ�Ԥ��յı߿���Ϣ
    box pred = get_yolo_box(x, biases, n, index, i, j, lw, lh, w, h, stride);    
    float iou = box_iou(pred, truth);

    // ������ǩ�е�x,y,w,h��Ϣ��ʹ֮��Ԥ���һ��
    // truth.x��truth.y��һ�����ֵ������ֱ�ӿ��Գ�������ͼ�óߴ�õ�������ͼ�ϵ�x��y��Ȼ���ȥcell��ƫ��
    float tx = (truth.x*lw - i);
    float ty = (truth.y*lh - j);
    // ����w��h�Ĵ�����ӳ�䵽��������������width��height�ĳߴ磬��ת��Ϊ�����anchor������ֵ
    float tw = log(truth.w*w / biases[2*n]);
    float th = log(truth.h*h / biases[2*n + 1]);
    // �ֱ����λ����ʧ 
    delta[index + 0*stride] = scale * (tx - x[index + 0*stride]);
    delta[index + 1*stride] = scale * (ty - x[index + 1*stride]);
    delta[index + 2*stride] = scale * (tw - x[index + 2*stride]);
    delta[index + 3*stride] = scale * (th - x[index + 3*stride]);
    return iou;
}

// ���ܣ�����������ʧ����
// ������������ݵ�ָ�룬��ʧ�����ָ�룬indexָʾ����������output�е���ʼ�洢λ��,classes��ʾ��Ԥ������
//       stride��ʾƫ������Ҳ����һ������ͼ��w*h��avg_cat���ݽ�����ֵΪ0�����һ��������ʾ�Ƿ�ʹ����focal_loss
void delta_yolo_class(float *output, float *delta, int index, int class_id, int classes, int stride, float *avg_cat, int focal_loss)
{
    int n;
    // ��˵��ʲô��������²�Ϊ0�أ�
    if (delta[index + stride*class_id]){
        delta[index + stride*class_id] = 1 - output[index + stride*class_id];
        if(avg_cat) *avg_cat += output[index + stride*class_id];
        return;
    }
    // Focal loss
    // ���������focal_loss��ִ�д�
    if (focal_loss) {
        // Focal Loss
        float alpha = 0.5;    // 0.25 or 0.5
        //float gamma = 2;    // hardcoded in many places of the grad-formula

        int ti = index + stride*class_id;
        float pt = output[ti] + 0.000000000000001F;
        // http://fooplot.com/#W3sidHlwZSI6MCwiZXEiOiItKDEteCkqKDIqeCpsb2coeCkreC0xKSIsImNvbG9yIjoiIzAwMDAwMCJ9LHsidHlwZSI6MTAwMH1d
        float grad = -(1 - pt) * (2 * pt*logf(pt) + pt - 1);    // http://blog.csdn.net/linmingan/article/details/77885832
        //float grad = (1 - pt) * (2 * pt*logf(pt) + pt - 1);    // https://github.com/unsky/focal-loss

        for (n = 0; n < classes; ++n) {
            delta[index + stride*n] = (((n == class_id) ? 1 : 0) - output[index + stride*n]);

            delta[index + stride*n] *= alpha*grad;

            if (n == class_id) *avg_cat += output[index + stride*n];
        }
    }
    else {
        // default
        for (n = 0; n < classes; ++n) {
            // ������Ӧ������ʧ����Ϊ1-�����÷֡�����λ������Ϊ0-���÷֣���Ҳ��LOGISTIC�󵼺�ʽ��Ӧ��
            delta[index + stride*n] = ((n == class_id) ? 1 : 0) - output[index + stride*n];
            if (n == class_id && avg_cat) *avg_cat += output[index + stride*n];
        }
    }
}

/****************************************************************************************************************************************
*  func������ĳ�����ο���ĳ��������l.output�е�������һ�����ο������x,y,w,h,c,C1,C2...,Cn��Ϣ��ǰ�ĸ����ڶ�λ�������Ϊ���ο�������  *
*        �����Ŷ���Ϣc�������ο��д�������ĸ���Ϊ��󣬶�C1��CnΪ���ο���������������ֱ�������n������ĸ��ʡ������������ȡ�þ��ο�   *
*        �׸���λ��ϢҲ��xֵ��l.output����������ȡ�þ��ο����Ŷ���Ϣc��l.output�е���������ȡ�þ��ο�����������ʵ��׸�����Ҳ��C1ֵ��   *
*        �����������ǻ�ȡ���ο��ĸ�������������ȡ�����������entry��ֵ����Щ��forward_yolo_layer()�����ж����õ�������l.output�Ĵ洢    *
*        ��ʽ����entry=0ʱ�����ǻ�ȡ���ο�x������l.output�е���������entry=4ʱ�����ǻ�ȡ���ο����Ŷ���Ϣc��l.output�е���������entry=5  *
*        ʱ�����ǻ�ȡ���ο��׸���������C1��l.output�е�������������Բο�forward_yolo_layer()�е��ñ�����ʱ��ע��.                      *                                                                                                      *
*  args l: ��ǰyolo_layer                                                                                                               *
*  args batch����ǰ��Ƭ������batch�еĵڼ��ţ���Ϊl.output�а�������batch�����������Ҫ��λĳ��ѵ��ͼƬ������ڶ������е�ĳ�����ο򣬵� *
*              Ȼ��Ҫ�ò���.                                                                                                            *
*  args location�����������˵ʵ�����о����߲����������������������ȡn��loc��ֵ�����n���Ǳ�ʾ�����еĵڼ���Ԥ����ο򣨱���ÿ������ *
*                 Ԥ��5�����ο���ônȡֵ��Χ���Ǵ�0~4����loc����ĳ��ͨ���ϵ�Ԫ��ƫ�ƣ�yolo_layer�����ͨ����Ϊ                        *
*                 l.out_c = (classes + 4 + 1)��,����˵����û��˵���ף��ⶼ��l.output�Ĵ洢�ṹ��أ���������ϸע���Լ�����˵������֮��  *                                *
*                 �鿴һ�µ��ñ������ĸ�����forward_yolo_layer()��֪���ˣ�����ֱ������n��j*l.w+i�ģ�û�б�Ҫ����location��������������  *
*                 ����һ��n��loc.                                                                                                       *
*  args entry�������ƫ��ϵ���������������������Ҫ����l.output�Ĵ洢�ṹ�ˣ���������ϸע���Լ�����˵��                                 *
*  details��l.output��������Ĵ洢�����Լ��洢��ʽ�Ѿ��ڶ���ط�˵���ˣ��ٶ�����ֶ�����ͼ��˵�����˴���                                *
*           ��Ҫ���¼��䣬��Ϊ����Ĳο�ͼ��˵����l.output�д洢������batch��ѵ�������ÿ��ѵ��ͼƬ�������                             *
*           l.out_w*l.out_h������ÿ�������Ԥ��l.n�����ο�ÿ�����ο���l.classes+4+1��������                                       *
*           �����һ������ͨ����Ϊl.n*(l.classes+4+1)�����������������������ά�����Ǹ�ʲô���ӵġ�                                   *
*           չ��һά����洢ʱ��l.output�������ȷֳ�batch����Σ�ÿ����δ洢��һ��ѵ��ͼƬ�������������һ��ϸ�֣�                     *
*           ȡ���е�һ��η������ô���д洢�˵�һ��ѵ��ͼƬ�����������Ԥ��ľ��ο���Ϣ��ÿ������Ԥ����l.n�����ο�                   *
*           �洢ʱ��l.n�����ο��Ƿֿ��洢�ģ�Ҳ�����ȴ洢���������еĵ�һ�����ο򣬶���洢���������еĵڶ������ο�                   *
*           �������ƣ����ÿ��������Ԥ��5�����ο�����Լ�������һ��ηֳ�5���жΡ�����ϸ�֣�5���ж���ȡ��                             *
*           һ���ж�������������ж��а��У���l.out_w*l.out_h�����񣬰��д洢�����δ洢������ѵ��ͼƬ�������������                     *
*           �ĵ�һ�����ο���Ϣ��Ҫע����ǣ�����жδ洢��˳�򲢲��ǰ��������洢ÿ�����ο��������Ϣ��                                  *
*           �����ȴ洢���о��ο��x�����������е�y,Ȼ�������е�w,����h��c�����ĵĸ�������Ҳ�ǲ�ֽ��д洢��                           *
*           ������һ���Ӵ洢��һ�����ο�������ĸ��ʣ������ȴ洢��������������һ��ĸ��ʣ��ٴ洢�����ڶ���ĸ��ʣ�                      *
*           ������˵��һ�ж����ȴ洢��l.out_w*l.out_h��x��Ȼ����l.out_w*l.out_h��y��������ȥ��                                          *
*           �����l.out_w*l.out_h��C1�����ڵ�һ��ĸ��ʣ���C1��ʾ���������ƣ���l.out_w*l.out_h��C2,...,                                 *
*           l.out_w*l.out_h*Cn�����蹲��n�ࣩ�����Կ��Լ������жηֳɼ���С�Σ�����Ϊx,y,w,h,c,C1,C2,...Cn                              *
*           С�Σ�ÿС�εĳ��ȶ�Ϊl.out_w*l.out_h.���ڻع����������������������batch���Ǵ�ε�ƫ�������ӵڼ�����ο�ʼ����Ӧ�ǵڼ�     *
*           ��ѵ��ͼƬ������location����õ���n�����жε�ƫ�������ӵڼ����жο�ʼ����Ӧ�ǵڼ������ο򣩣�                               *
*           entry����С�ε�ƫ�������Ӽ���С�ο�ʼ����Ӧ���������ֲ�����x,c����C1������loc�������Ķ�λ��                               *
*           ǰ��ȷ���õڼ�����еĵڼ��ж��еĵڼ�С�ε��׵�ַ��loc���ǴӸ��׵�ַ������loc��Ԫ�أ��õ����ն�λ                          *
*           ĳ�����������x��c��C1��������ֵ������l.output�д洢������������ʾ���������ֻ����һ��ѵ��ͼƬ�������                      *
*           ���batchֻ��Ϊ1��������l.out_w=l.out_h=2,l.classes=2����                                                                   *
*           xxxxyyyywwwwhhhhccccC1C1C1C1C2C2C2C2-#-xxxxyyyywwwwhhhhccccC1C1C1C1C2C2C2C2��                                               *
*           n=0��λ��-#-��ߵ��׵�ַ����ʾÿ������Ԥ��ĵ�һ�����ο򣩣�n=1��λ��-#-�ұߵ��׵�ַ����ʾÿ������Ԥ��ĵڶ������ο�  *
*           entry=0,loc=0��ȡ����x���������һ�ȡ���ǵ�һ��xҲ��l.out_w*l.out_h�������е�һ�������е�һ�����ο�x������������             *
*           entry=4,loc=1��ȡ����c���������һ�ȡ���ǵڶ���cҲ��l.out_w*l.out_h�������еڶ��������е�һ�����ο�c������������             *
*           entry=5,loc=2��ȡ����C1���������һ�ȡ���ǵ�����C1Ҳ��l.out_w*l.out_h�������е����������е�һ�����ο�C1������������          *
*           ���Ҫ��ȡ��һ�������е�һ�����ο�w�����������أ�����Ѿ���ȡ����xֵ����������Ȼ��x����������3*l.out_w*l.out_h���ɻ�ȡ����  *
*           ���Ҫ��ȡ�����������е�һ�����ο�C2�����������أ�����Ѿ���ȡ����C1ֵ����������Ȼ��C1����������l.out_w*l.out_h���ɻ�ȡ���� *
*           ���Ͽ�֪��entry=0ʱ,��ƫ��0��С�Σ��ǻ�ȡx��������entry=4,�ǻ�ȡ���Ŷ���Ϣc��������entry=5���ǻ�ȡC1������.                 *
*           l.output�Ĵ洢��ʽ���¾������������˾���˵���Ѿ�������ˣ������ӻ�Ч���վ�����ͼ��˵����                                    *
****************************************************************************************************************************************/
static int entry_index(layer l, int batch, int location, int entry)
{
    int n =   location / (l.w*l.h);
    int loc = location % (l.w*l.h);
    // ���¹�ʽ���ͣ�batch*l.outputs l.outputs��ʾ���Ǳ���һ��ͼƬ�����ά�ȣ�����batch���ʾ�ǵڼ���ͼƬ��ƫ��
    // n*l.w*l.h*(4+l.classes+1) ����n��ʾ�ڼ�������yolo3Ϊ��������n=2���ʱ�õ����ǵڶ�����ƫ����
    // entry*l.w*l.h ÿ����Ϣռ��λ��Ϊl.w*l.h ��Ҫ��ĵڼ�����Ϣ��ƫ�����ͳ��Գ���
    // loc��ʾ���յ�С��ƫ�ƣ���������x�еĵڼ���x
    // ���ٲ���⣬���Խ��forward_yolo_layer���Ĳ�ѭ������˵�����
    return batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h + loc;
}

/***************************************************************************************************************
*  func: �Ӵ洢���ο���Ϣ�Ĵ������У���ȡĳһ�����ο�λ��Ϣ�����أ�f�������������ĳһ��ʼ��ַ��.            *
*  args f�����ο���Ϣ��ÿ�����ο���5������ֵ���˺�������ȡ����4�����ڶ�λ�Ĳ���x,y,w,h����������������ţ�   *
*  args stride: ��ȣ���������Խȡֵ���˴�Ҫ���net.truth�Ĵ洢�ṹ���                                        *
***************************************************************************************************************/
static box float_to_box_stride(float *f, int stride)
{
    // f�д洢ÿһ�����ο���Ϣ��˳��Ϊ: x, y, w, h, class_index���������ȡǰ�ĸ���
    // Ҳ�����ο�Ķ�λ��Ϣ�����һ�������������Ϣ���ڴ˴���ȡ
    box b = { 0 };
    b.x = f[0];
    b.y = f[1 * stride];
    b.w = f[2 * stride];
    b.h = f[3 * stride];
    return b;
}


/*****************************************************************************************************************************
*  func�����yolo���ǰ�򴫵�                                                                                                *
*  args l : ��������ò���ָ��                                                                                               *
*  args state����һ��������Ϣ                                                                                              *
*  details����������ε�����entry_index()��������ʹ�õĲ���������ͬ�����������һ��������ͨ�����һ��������                  *
*           ����ȷ����yolo_layer���l.output�����ݴ洢��ʽ��Ϊ�������������豾���������l.w = 2, l.h= 3,                     *
*           l.n = 2, l.classes = 2, l.c = l.n * (4 + l.classes + 1) = 14,                                                    *
*           l.output�д洢�����о��ο����Ϣ������ÿ�����ο����4����λ��Ϣ����x,y,w,h��һ�����Ŷȣ�confidience��            *
*           ����c���Լ��������ĸ���C1,C2�������У������ֻ���������l.classes=2������ôһ������ͼƬ���ջ���              *
*           l.w*l.h*l.n�����ο�l.w*l.h��Ϊ����ͼ�񻮷ֲ�����ĸ�����ÿ������Ԥ��l.n�����ο򣩣���ô                        *
*           l.output�д洢��Ԫ�ظ�������l.w*l.h*l.n*(4 + 1 + l.classes)����ЩԪ��ȫ�������һά����                          *
*           ����ʽ�洢��l.output�У��洢��˳��Ϊ��                                                                           *
*           xxxxxx-yyyyyy-wwwwww-hhhhhh-cccccc-C1C1C1C1C1C1C2C2C2C2C2C2-                                                     *
*           ##-xxxxxx-yyyyyy-wwwwww-hhhhhh-cccccc-C1C2C1C2C1C2C1C2C1C2C1C2                                                   *
*           ����˵�����£�-##-�����ֳ����Σ����ҷֱ��Ǵ�����������ĵ�1��box�͵�2��box����Ϊl.n=2����ʾÿ������Ԥ������box�� *
*           �ܹ���l.w*l.h�������Ҵ洢ʱ�������������x,y,w,h,c��Ϣ�۵�һ����ƴ�����������xxxxxx��������Ϣ����l.w*l.h=6����*
*           ��Ϊÿ����l.classes��������𣬶���Ҳ�Ǻ�xywhһ����ÿһ�඼���д洢���ȴ洢l.w*l.h=6��C1�࣬����洢6��C2�࣬    *
*           ��Ϊ�����ע�Ϳ��Ժ����е����ע�ͣ�ע�ⲻ��C1C2C1C2C1C2C1C2C1C2C1C2��ģʽ�����ǽ����е����𿪷ֱ��д洢���� *
*           ���ŶȲ���c��ʾ���Ǹþ��ο��ڴ�������ĸ��ʣ���C1��C2�ֱ��ʾ���ο��ڴ�������ʱ��������1������2�ĸ��ʣ�          *
*           ���c*C1���þ��ο��ڴ�������1�ĸ��ʣ�c*C2���þ��ο��ڴ�������2�ĸ���                                             *
*****************************************************************************************************************************/
void forward_yolo_layer(const layer l, network_state state)
{
    int i,j,b,t,n;
    // memccpy����ԭ�ͣ�void *memcpy(void *dest, const void *src, size_t n);
    // memcpyָ����c��c++ʹ�õ��ڴ濽��������memcpy�����Ĺ����Ǵ�Դsrc��ָ���ڴ��ַ����ʼλ�ÿ�ʼ����n���ֽڵ�Ŀ��dest��ָ���ڴ��ַ����ʼλ���С�
    // ��net.input�е�Ԫ��ȫ��������l.output��
    // ���state.input��������һ������,l.output�Ǳ�����������,�����������ǿ���Ԫ�صĳ��ȣ�ע�������������
    memcpy(l.output, state.input, l.outputs*l.batch*sizeof(float));

    // ���#ifndefԤ����ָ��û�б�Ҫ�õģ���Ϊforward_yolo_layer()��������Ͷ�Ӧû�ж���gpu��ģ����Կ϶���ִ�����е����,
    // ���е�����������Ϊ�˼���yolo_layer������l.output
    // ��һ���ܽ����������þ��Ƕ�l.output�е�xy c��C������LOGISTIC�����
#ifndef GPU
    // ����batch�е�ÿ��ͼƬ��l.output��������batchѵ��ͼƬ��Ӧ�������
    for (b = 0; b < l.batch; ++b){
        // ע��yolo_layer���е�l.n������ÿ��cell grid��������Ԥ��ľ��ο���������Ǿ�����о���˵ĸ�����,Ҳ����ÿ��yolo��anchor�ø���
        for(n = 0; n < l.n; ++n){
            // ��ȡ ĳһ�ж��׸�x�ĵ�ַ���жεĺ���ο�entry_idnex()������ע�ͣ����˴���������ѭ���������е����룬ֱ����Ӧ����Ҫ�Ĳ�ģ�
            // ������Ҫ�������l.w��l.h������ÿһ�����񣩣���ʵ���ϲ�����Ҫ����Ϊÿ��ѭ�����䶼�ᴦ��һ���ж���ĳһС�ε����ݣ���һС������
            // �Ͱ���������������ݡ����紦���1���ж�������x��y���ֱ���l.w*l.h��x��y��.
            int index = entry_index(l, b, n*l.w*l.h, 0);
            // ע��ڶ���������2*l.w*l.h��Ҳ���Ǵ�l.output��index��������ʼ����֮��2*l.w*l.h��Ԫ�ؽ���logistic���������Ҳ���Ƕ�
            // һ���ж������е�x,y����logistic��������֮����ֻ��x,y�����������w,h��������Ҫ��w��h�ļ��㹫ʽ�йأ�
            // activate_array������src/activation.c ���ζ�l.output�д�index�������2*l.w*l.h��Ԫ�ؽ��м���������ΪLOGISTIC
            activate_array(l.output + index, 2*l.w*l.h, LOGISTIC);

            // ������һ�����˴��ǻ�ȡһ���ж����׸����Ŷ���Ϣcֵ�ĵ�ַ������Ը��ж������е�cֵ�����ж��ڹ���l.w*l.h��cֵ������logistic���������
            index = entry_index(l, b, n*l.w*l.h, 4);
            activate_array(l.output + index, (1+l.classes)*l.w*l.h, LOGISTIC);
        }
    }
#endif

    // memset����ԭ�ͣ�void *memset(void *s, int ch, size_t n);
    // ��s�е�ǰλ�ú����n���ֽ� ��typedef unsigned int size_t ���� ch �滻������ s ��
    // memset����������һ���ڴ�������ĳ��������ֵ�����ǶԽϴ�Ľṹ�������������������һ����췽��
    // �˴��������ʼ������l.delta������Ԫ�أ�����l.outputs*l.batch��Ԫ�أ�ÿ��Ԫ��sizeof(float)���ֽڣ�����
    memset(l.delta, 0, l.outputs * l.batch * sizeof(float));
    // �������ѵ�����̣��򷵻ز���ִ���������䣨ǰ������������Ҳ����������������ʱ�Ͳ���Ҫִ������ѵ��ʱ�Ż��õ�������ˣ�
    if(!state.train) return;


    float avg_iou = 0;
    float recall = 0;
    float recall75 = 0;
    float avg_cat = 0;
    float avg_obj = 0;
    float avg_anyobj = 0;    // һ��ѵ��ͼƬ����Ԥ����ο��ƽ�����Ŷȣ����ο��к�������ĸ��ʣ����ò���û��ʵ���ô��������������ӡ
    int count = 0;
    int class_count = 0;
    // ���Ƚ�loss��0
    *(l.cost) = 0;

    // ���ѭ������ÿ��ͼƬ
    for (b = 0; b < l.batch; ++b) {
        // ����ѭ������ÿ��ͼƬ��������
        for (j = 0; j < l.h; ++j) {
            // ����һ��ͼƬ��������
            for (i = 0; i < l.w; ++i) {
                // �˴���l.nͬ�����ᵽ��һ������ʾÿ��cellԤ��Ŀ�ĸ�������yolov3��˵��3��.��������δ���ÿһ����
                for (n = 0; n < l.n; ++n) {
                    // �õ���b��batch�У���Ϊj,��Ϊi�ĵ�n�ࣨ�ܹ�3�ࣩ���x��������������ܱȽϻ�ɬ������ϸ���ͣ���֪���Բ��ԣ��ðɣ������Լ�
                    // ���entry_index���������� n=location/(w*h)  (�������ҵ�l.w��l.h�ͼ�д��)���Ǵ�ʱ���빫ʽʱ�õ���n��ֵ�ͺ�i��ȣ�Ҳ���ǵڼ���
                    // ���ƫ�ơ��ٿ�loc,loc=location%(w*h) ,��ʵ�δ��빫ʽ�У�loc=j*w+i,�ⲻ����һ��ͼƬ��ά���أ�һ��ͨ��������Ϊj����Ϊi��ƫ����
                    // �����loc���index�Ĺ�ʽ��l.output�Ĵ洢ģʽ������index=batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h + loc;
                    // batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h�õ�������w*h��X����ʼ��������Ϊ������һ������Ϊ0��˵���ǻ��X����ʼ��������
                    // ����ټ���loc�͸պ���w*h��X�еĵ�loc������֪��������û�н������ǰ��Ҳ�����Ҫ���entry_index�����еĴ��������ʵ���о�
                    int box_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 0);
                    // box���ݽṹ������include/darknet.h�� �䶨��Ϊ��
                    // typedef struct{
                    //	  float x, y, w, h;
                    // } box;
                    // get_yolo_box���������ļ� �õ�һ��yolo������ĵ�j�У���i�е�cell��Ԥ��ĵ�n�����BBox����
                    box pred = get_yolo_box(l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, state.net.w, state.net.h, l.w*l.h);
                    // ���IoU������ֵ0
                    float best_iou = 0;
                    int best_t = 0;
                    // l.max_boxesֵΪ90���鿴darknet.h�й���max_boxes������ע�;�֪���ˣ�ÿ��ͼƬ����ܹ���90��ʵ����;
                    // ��Ҫ˵����һ���ǣ��˴���˵��ദ��90�����ο���ָ��ʵֵ�У�һ��ͼƬ���е������ʵ�����ǩ����
                    // Ҳ����ʵ��������ο�������Ϊ90,������ģ��Ԥ����������90��������ע�ͣ�һ��ͼƬ����ֳ�7*7������
                    // ÿ������Ԥ���������ο��Ǿ���90���ˣ����Բ���ָģ��ֻ��Ԥ��90����ģ������Ծ���Ԥ���ľ��ο�
                    // ֻ����Ĭ��һ��ͼƬ�����ʹ���90������ı�ǩ������֮������˳����̡��˳����������㣬���ȵ�һ�����
                    // �����forѭ���ˣ�
                    // ִ�������ѭ������һ���¿��Կ϶���best_iou�м�¼����һ��cell��1��anchor��Ԥ��Ŀ�������Ground truth֮������IOU��best_t��¼��
                    // �����Ground truth�ı�ţ�������
                    for(t = 0; t < l.max_boxes; ++t){
                        // ͨ����λ����ȡÿһ����ʵ���ο����Ϣ��net.truth�洢���������������ͼƬ����ʵ���ο���Ϣ��һ������һ��batch��ѵ��ͼƬ����
                        // net.truth��Ϊ��һ����������׵�ַ��l.truths������ÿһ��ͼƬ���е���ʵֵ�����������ɲο�darknet.h�е�truths�����е�ע�ͣ���
                        // b��batch���Ѿ�������ͼƬ��ͼƬ��������5��ÿ����ʵ���ο���Ҫ5������ֵ��Ҳ��ÿ�����ο���ֵ��5����������t�Ǳ���ͼƬ�Ѿ�����
                        // ���ľ��ο�ĸ�����ÿ��ͼƬ��ദ��90���򣩣�����������Ĳ���֮������������λ��ȡ��Ӧ���ο���ʵֵ�Ĵ���Ͳ����ˡ�
                        // float_to_box_stride������net.truth��ȡ����ǩ����Ϣ b*l.truthΪͼƬ��ƫ�ƣ�t*(4+1)Ϊ���ƫ��
                        box truth = float_to_box_stride(state.truth + t*(4 + 1) + b*l.truths, 1);
                        // ��ȡ��ʵ���ο��Ӧ�������Ϣ
                        int class_id = state.truth[t*(4 + 1) + b*l.truths + 4];
                        // �򵥵����
                        if (class_id >= l.classes) {
                            printf(" Warning: in txt-labels class_id=%d >= classes=%d in cfg-file. In txt-labels class_id should be [from 0 to %d] \n", class_id, l.classes, l.classes - 1);
                            getchar();
                            continue; // if label contains class_id more than number of classes in the cfg-file
                        }
                        // ���if����������ж�һ���Ƿ��ж�����ʵ���ο�ֵ��ÿ�����ο���5������,float_to_box_strideֻ��ȡ���е�4����λ������
                        // ֻҪ��֤x��ֵ��Ϊ0,�ǿ϶���4������ֵ����ȡ���ˣ�Ҫôȫ����ȡ���ˣ�Ҫôһ��Ҳû�У������⣬��Ϊ������д����ÿ��ͼƬ����90�����ο�
                        // ��ô��ЩͼƬû����ô����ο򣬾ͻ����û�ж������������ʱ��xΪ0���ͻ���������
                        if(!truth.x) break;  // continue;
                        // box_iou������src/box.c ��ȡ����ʵ��ǩ���ζ�λ�������ģ�ͼ����ľ��ο���IoU������ο�box_iou()����ע��
                        float iou = box_iou(pred, truth);
                        // �ҳ�����IoUֵ�Ͷ�Ӧ���ڱ�ǩ�е�����
                        if (iou > best_iou) {
                            best_iou = iou;
                            best_t = t;
                        }
                    }
                    // ��ȡ��ǰ����cell������������Ŷ���Ϣc���þ��ο��е�ȷ��������ĸ��ʣ���l.output�е�����ֵ
                    int obj_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 4);
                    // ����ÿ��Ԥ����ο�����Ŷ�c��Ҳ��ÿ�����ο��к�������ĸ��ʣ�
                    avg_anyobj += l.output[obj_index];

                    // �����ݶȣ�delta�б���ľ��Ǳ�����ݶ�ֵ
                    // �������ignore_thresh, ��ô���ԣ���˼�Ǽ����ȷ�ˣ������Ŷȵ��ݶ�Ϊ0
                    // ���С��ignore_thresh��target = 0
                    // diff = -gradient = target - output������gradient=output-target
                    // Ϊʲô����ʽ���ο���https://xmfbit.github.io/2018/04/01/paper-yolov3/
                    l.delta[obj_index] = 0 - l.output[obj_index];
                    // ����90��ѭ��ʹ�ñ����ο��Ѿ���ѵ��ͼƬ������90����90��ֻ�����ֵ������û����ô�ࣩ��ʵ���α�ǩ�����˶Աȣ�ֻҪ����90����
                    // �ҵ�һ����ʵ���α�ǩ���Ԥ����ο��iou����ָ������ֵ�����ж��ÿ�����ȷ�ˣ������Ŷȵ��ݶ�Ϊ0
                    if (best_iou > l.ignore_thresh) {
                        l.delta[obj_index] = 0;
                    }
                    // best_iou�������1 �����Դ˶δ������������l.truth_thresh��ֵ�Ļ���������ִ�еģ��ּ����ֵΪ0.9������һ��ִ�еĻ��ᣬ��ȥ���پ������
                    // �˴���ģ��˫IOU��ֵ��
                    if (best_iou > l.truth_thresh) {
                        // ��ʱ��best_iou�������0.9��˵���ü�����ĳ����ʵ��֮����ص��Ⱥܸߺܸߣ�˵��������п�����Ŀ������Ƕ���
                        // ��˴������Ŷ���Ϣ����ʧӦ�ú�С�����о�Ҳ��������ǰ��������Ϊ0�����԰���ʽtarget-output ���㣬����ʱtarget����1
                        l.delta[obj_index] = 1 - l.output[obj_index];

                        int class_id = state.truth[best_t*(4 + 1) + b*l.truths + 4];

                        if (l.map) class_id = l.map[class_id];
                        // �ҵ������Ϣ����ʼ�洢λ��
                        int class_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 4 + 1);
                        // ���������ʧ�����������Ŷ���ʧ���ƣ�ֻ�Ǵ˴�ҪΪ���λ�ø�ֵ
                        delta_yolo_class(l.output, l.delta, class_index, class_id, l.classes, l.w*l.h, 0, l.focal_loss);
                        // ��������IOU�ı�ǩ�����Ϣ�����ڼ���λ����ʧ
                        box truth = float_to_box_stride(state.truth + best_t*(4 + 1) + b*l.truths, 1);
                        // ����λ����ʧ��Ϊ��scaleΪ��2-truth.w*truth.h��
                        delta_yolo_box(truth, l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, state.net.w, state.net.h, l.delta, (2-truth.w*truth.h), l.w*l.h);
                    }
                }
            }
        }

        // ǰ��ѭ�������ÿһ��Ԥ����������ʧ������ѵ�����ÿ����ǩ�������ʧ
        for(t = 0; t < l.max_boxes; ++t){
            // truth�б����˱�ǩ���λ����Ϣ
            box truth = float_to_box_stride(state.truth + t*(4 + 1) + b*l.truths, 1);
            // �õ���ǩ��������Ϣ
            int class_id = state.truth[t*(4 + 1) + b*l.truths + 4];
            // �������ʱ����������
            if (class_id >= l.classes) continue; // if label contains class_id more than number of classes in the cfg-file
            // ע��ͬ��
            if(!truth.x) break;  // continue;
            float best_iou = 0;
            int best_n = 0;
            // i��ֵΪ��ǩ���w������ͼ�ϵľ���ֵ��jΪh�ľ���ֵ
            i = (truth.x * l.w);
            j = (truth.y * l.h);
            box truth_shift = truth;

            truth_shift.x = truth_shift.y = 0;
            // ���ѭ��ò�����ҳ�����ʵ����״��ӽ���anchor
            for(n = 0; n < l.total; ++n){
                box pred = {0};
                // ȡ����Ӧanchor��w��h
                pred.w = l.biases[2*n]/ state.net.w;
                pred.h = l.biases[2*n+1]/ state.net.h;
                float iou = box_iou(pred, truth_shift);
                if (iou > best_iou){
                    best_iou = iou;
                    best_n = n;
                }
            }
            // int_index������util.c �ҳ���������a�е�Ԫ��val���ڵ�λ�ã�����������ֵ,���ⷵ�ص���
            // �ڸ�yolo���У������ҵ�������ʵ����״��ӽ���anchor������,�����������������ֵΪl.n-1
            int mask_n = int_index(l.mask, best_n, l.n);
            // �����Ӧanchor���ڣ���mask_n>=0
            if(mask_n >= 0){
                //  �ҵ���Ӧanchor�������ֵ��x����ʼ����
                int box_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 0);
                // ����anchor�ͱ�ǩ��֮����ݶ�
                float iou = delta_yolo_box(truth, l.output, l.biases, best_n, box_index, i, j, l.w, l.h, state.net.w, state.net.h, l.delta, (2-truth.w*truth.h), l.w*l.h);
                // �õ�������Ŷ�
                int obj_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 4);
                avg_obj += l.output[obj_index];
                l.delta[obj_index] = 1 - l.output[obj_index];

                int class_id = state.truth[t*(4 + 1) + b*l.truths + 4];
                if (l.map) class_id = l.map[class_id];
                int class_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 4 + 1);
                delta_yolo_class(l.output, l.delta, class_index, class_id, l.classes, l.w*l.h, &avg_cat, l.focal_loss);

                ++count;
                ++class_count;
                if(iou > .5) recall += 1;
                if(iou > .75) recall75 += 1;
                avg_iou += iou;
            }
        }
    }
    // mag_array��������l.delta��ÿ��Ԫ�ص�ƽ���ͣ��ٿ���
    *(l.cost) = pow(mag_array(l.delta, l.outputs * l.batch), 2);
    printf("Region %d Avg IOU: %f, Class: %f, Obj: %f, No Obj: %f, .5R: %f, .75R: %f,  count: %d\n", state.index, avg_iou/count, avg_cat/class_count, avg_obj/count, avg_anyobj/(l.w*l.h*l.n*l.batch), recall/count, recall75/count, count);
}

void backward_yolo_layer(const layer l, network_state state)
{
   axpy_cpu(l.batch*l.inputs, 1, l.delta, 1, state.delta, 1);
}

/*****************************************************************************************
*  func: �������������width��heightΪsize����Ԥ��ı߿���Ϣx��y,w,hת������resize����   *
*        ��letter_box���м�ͼ��width��height��sizeΪ��׼�ı߿���Ϣ����ΪԤ��ֵx,y,w,h    *
*        ����������������width��height�����˹�һ��������letter_box��ͼƬ��width��height*
*        ����������������£�w>h�������ң�w<h��������ֵΪ0.5�Ĳ��ߣ���Ҫ����ת��         *
*  args dets����Ҫת���ı߿��б�                                                         *
*  args n: ����ܸ���                                                                    *
*  args w/h: ԭʼͼƬ��width��height                                                     *
*  args netw/neth: ���������width��height                                               *
*  args relative: ָʾ�߿���Ϣ�е�x,y,w,h�Ƿ������ֵ������һ�����ֵ��Ĭ��Ϊ1           *
*****************************************************************************************/
void correct_yolo_boxes(detection *dets, int n, int w, int h, int netw, int neth, int relative, int letter)
{
    int i;
    // �˴�new_w��ʾ����ͼƬ��ѹ���������������С��letter_box�е�width,new_h��ʾ��leitter_box�е�
    // height,��1280*720������ͼƬΪ�����ڽ���letter_box�Ĺ����У���Ϊw>h��1280>720��������ԭͼ��
    // resize���width�����������widthΪ��׼���������Ϊ416����ôresize��Ķ�ӦheightΪ720*416/1280
    // ����heightΪ234��������234�����¿��ಿ������Ϊ��������֮ǰ����˸���ֵ0.5�����һ��Ҫ����new_w
    // ��new_h��ʵ������   ��Ȼ������ÿ�ָʾ����letterָʾ�Ƿ������Ƕ��
    int new_w=0;
    int new_h=0;
    if (letter) {
        // ���w>h˵��resize��ʱ������width/ͼ���widthΪresize�����ģ��ȵõ��м�ͼ��width,�ٸ��ݱ����õ�height
        if (((float)netw / w) < ((float)neth / h)) {
            new_w = netw;
            new_h = (h * netw) / w;
        }
        else {
            new_h = neth;
            new_w = (w * neth) / h;
        }
    }
    else {
        // ���ָʾû��Ƕ�룬��Ϊ���������width�� height����������AB�����߿��ǵ���Ƕ���ϵͳҲûʲôӰ���ԭ���
        new_w = netw;
        new_h = neth;
    }
    for (i = 0; i < n; ++i){
        box b = dets[i].bbox;
        // �˴��Ĺ�ʽ�ܲ�����⻹�ǽ�����������ӣ�����new_w=416,new_h=234,��Ϊresize����wΪ����ѹ����
        // ����x�����width�ı������䣬��b.y��ʾy�����ͼ��߶ȵı������ڽ�����һ����ת��֮ǰ��b.y��ʾ
        // ����Ԥ����y�������������height�ı�ֵ��Ҫת���������letter_box��ͼ���height�ı�ֵʱ����Ҫ��
        // �����y��letter_box�е�������꣬��(b.y - (neth - new_h)/2./neth)���ٳ��Ա���
        b.x =  (b.x - (netw - new_w)/2./netw) / ((float)new_w/netw);
        b.y =  (b.y - (neth - new_h)/2./neth) / ((float)new_h/neth);
        b.w *= (float)netw/new_w;
        b.h *= (float)neth/new_h;
        // �����ǩ���ֵ�������ֵ������Ҫת��Ϊ����ֵ
        if(!relative){
            b.x *= w;
            b.w *= w;
            b.y *= h;
            b.h *= h;
        }
        dets[i].bbox = b;
    }
}

/******************************************************************
*  func: ͳ��yolo���������Ŀ���confidenceֵ��thresh��Ŀ����   *
*  args l: yolo���ָ��                                           *
*  args thresh����ֵ                                              *
******************************************************************/
int yolo_num_detections(layer l, float thresh)
{
    int i, n;
    int count = 0;
    // �˴�l.w��l.h��ʾ�ò������ά�ȣ���ò������ά��һ��
    for (i = 0; i < l.w*l.h; ++i){
        // l.n��ʾ�ò�ÿ��cellԤ����ٸ��򣬶���yolov3��˵Ϊ3
        for(n = 0; n < l.n; ++n){
            // entry_index���������ļ� ��λĿ��confidence����ʼ����
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4);
            if(l.output[obj_index] > thresh){
                // ��Ŀ���confidenceֵ��thresh��ʱ�����������1
                ++count;
            }
        }
    }
    return count;
}

void avg_flipped_yolo(layer l)
{
    int i,j,n,z;
    float *flip = l.output + l.outputs;
    for (j = 0; j < l.h; ++j) {
        for (i = 0; i < l.w/2; ++i) {
            for (n = 0; n < l.n; ++n) {
                for(z = 0; z < l.classes + 4 + 1; ++z){
                    int i1 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + i;
                    int i2 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + (l.w - i - 1);
                    float swap = flip[i1];
                    flip[i1] = flip[i2];
                    flip[i2] = swap;
                    if(z == 0){
                        flip[i1] = -flip[i1];
                        flip[i2] = -flip[i2];
                    }
                }
            }
        }
    }
    for(i = 0; i < l.outputs; ++i){
        l.output[i] = (l.output[i] + flip[i])/2.;
    }
}

/******************************************************************************************
*  func: ��ȡԤ����confidence=objectness,�߿���Ϣ�������ʣ���������Ҫ����abjectness *
*        �����ú���correct_yolo_boxes�Ա߿���Ϣ���о�������ϸע�ͼ����崦               *
*  args l���������yolo�㣬��output������Ԥ���������ʵ���Ϣ                           *
*  args w: ԭʼͼƬ��width                                                                *
*  args h: ԭʼͼƬ��height                                                               *
*  args netw/neth: ���������width��height                                                *
*  args thresh: ���ҵ����Ƿ�Ϊ��Ч�����ֵ                                                *
*  args map: yolov3��û��ʹ��                                                             *
*  args relative: ָʾ�߿���Ϣ�е�x,y,w,h�Ƿ������ֵ������һ�����ֵ��Ĭ��Ϊ1            *
*  args dets: Ԥ����ָ�룬���ô���,�˴������Ŀ�ͨ��dets����                           *
******************************************************************************************/
int get_yolo_detections(layer l, int w, int h, int netw, int neth, float thresh, int *map, int relative, detection *dets, int letter)
{
    //printf("\n l.batch = %d, l.w = %d, l.h = %d, l.n = %d \n", l.batch, l.w, l.h, l.n);
    int i,j,n;
    float *predictions = l.output;
    // ���Ե�ʱ��batch������Ϊ1���ѵ��������ó�2������Ч��
    if (l.batch == 2) avg_flipped_yolo(l);
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){
        int row = i / l.w;
        int col = i % l.w;
        for(n = 0; n < l.n; ++n){
            // entry_index���������ļ� �ҵ�Ԥ����confidence����ʼ����
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4);
            // ��ȡ���Ŷ�
            float objectness = predictions[obj_index];
            //if(objectness <= thresh) continue;    // incorrect behavior for Nan values
            if (objectness > thresh) {
                //printf("\n objectness = %f, thresh = %f, i = %d, n = %d \n", objectness, thresh, i, n);
                // �õ�ÿ��Ԥ����BBox��Ϣ����ʼ����
                int box_index = entry_index(l, 0, n*l.w*l.h + i, 0);
                // get_yolo_box���������ļ� ��ȡĳ�����ο��4����λ��Ϣ����������ľ��ο�������l.output�л�ȡ�þ��ο�Ķ�λ��Ϣx,y,w,h��
                dets[count].bbox = get_yolo_box(predictions, l.biases, l.mask[n], box_index, col, row, l.w, l.h, netw, neth, l.w*l.h);
                dets[count].objectness = objectness;
                dets[count].classes = l.classes;
                for (j = 0; j < l.classes; ++j) {
                    // �˴���ô��ȡÿ�����ķ���÷��ǲ��ǲ�̫��Ч��
                    int class_index = entry_index(l, 0, n*l.w*l.h + i, 4 + 1 + j);
                    // prob��֤���Ĺ�ʽ�е�confidence*���÷�
                    float prob = objectness*predictions[class_index];
                    // ���ո�ֵ��dets��prob����Ҫ����ֵ���бȽ�
                    dets[count].prob[j] = (prob > thresh) ? prob : 0;
                }
                ++count;
            }
        }
    }
    // correct_yolo_boxes���������ļ� ��dets�еı߿���Ϣ����ת����������������崦
    // ת������dets�б������������������ʴ���thresh������߽�������Ѿ�ת����������������ͼƬ����ֵ
    correct_yolo_boxes(dets, count, w, h, netw, neth, relative, letter);
    return count;
}

#ifdef GPU

void forward_yolo_layer_gpu(const layer l, network_state state)
{
    //copy_ongpu(l.batch*l.inputs, state.input, 1, l.output_gpu, 1);
    simple_copy_ongpu(l.batch*l.inputs, state.input, l.output_gpu);
    int b, n;
    for (b = 0; b < l.batch; ++b){
        for(n = 0; n < l.n; ++n){
            int index = entry_index(l, b, n*l.w*l.h, 0);
            // y = 1./(1. + exp(-x))
            // x = ln(y/(1-y))  // ln - natural logarithm (base = e)
            // if(y->1) x -> inf
            // if(y->0) x -> -inf
            activate_array_ongpu(l.output_gpu + index, 2*l.w*l.h, LOGISTIC);    // x,y
            index = entry_index(l, b, n*l.w*l.h, 4);
            activate_array_ongpu(l.output_gpu + index, (1+l.classes)*l.w*l.h, LOGISTIC); // classes and objectness
        }
    }
    if(!state.train || l.onlyforward){
        //cuda_pull_array(l.output_gpu, l.output, l.batch*l.outputs);
        cuda_pull_array_async(l.output_gpu, l.output, l.batch*l.outputs);
        CHECK_CUDA(cudaPeekAtLastError());
        return;
    }

    float *in_cpu = (float *)calloc(l.batch*l.inputs, sizeof(float));
    cuda_pull_array(l.output_gpu, l.output, l.batch*l.outputs);
    memcpy(in_cpu, l.output, l.batch*l.outputs*sizeof(float));
    float *truth_cpu = 0;
    if (state.truth) {
        int num_truth = l.batch*l.truths;
        truth_cpu = (float *)calloc(num_truth, sizeof(float));
        cuda_pull_array(state.truth, truth_cpu, num_truth);
    }
    network_state cpu_state = state;
    cpu_state.net = state.net;
    cpu_state.index = state.index;
    cpu_state.train = state.train;
    cpu_state.truth = truth_cpu;
    cpu_state.input = in_cpu;
    forward_yolo_layer(l, cpu_state);
    //forward_yolo_layer(l, state);
    cuda_push_array(l.delta_gpu, l.delta, l.batch*l.outputs);
    free(in_cpu);
    if (cpu_state.truth) free(cpu_state.truth);
}

void backward_yolo_layer_gpu(const layer l, network_state state)
{
    axpy_ongpu(l.batch*l.inputs, 1, l.delta_gpu, 1, state.delta, 1);
}
#endif
