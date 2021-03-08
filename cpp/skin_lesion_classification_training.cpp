#include "data_generator/data_generator.h"
#include "models/models.h"
#include "utils/utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <cmath>

#include "ecvl/core/filesystem.h"
#include "eddl/serialization/onnx/eddl_onnx.h"
#include "eddl/losses/loss.h"

using namespace ecvl;
using namespace ecvl::filesystem;
using namespace eddl;
using namespace std;

class LWCategoricalCrossEntropy : public Loss
{
    const vector<float> weights_;
public:
    LWCategoricalCrossEntropy() = delete;
    LWCategoricalCrossEntropy(const vector<float>& weights) : Loss("wcategorical_cross_entropy"), weights_{ weights } {}

    void delta(Tensor* T, Tensor* Y, Tensor* D) override
    {
        tensor y_true = T->clone();
        tensor y_pred = Y->clone();
        y_true->toCPU();
        y_pred->toCPU();
        D->toCPU();
        //float eps = 10e-8;
        constexpr float eps = FLT_EPSILON;

    #pragma omp parallel for
        for (int b = 0; b < y_true->shape[0]; ++b) {
            for (int c = 0; c < y_true->shape[1]; ++c) {
                auto index = b * y_true->stride[0] + c;
                D->ptr[index] = weights_[c] * (-y_true->ptr[index]) * (1.0f / (y_pred->ptr[index] + eps));
            }
        }

        D->div_(static_cast<float>(D->shape[0]));
        //D->toDevice(T->device);
        D->toGPU();
    }

    float value(Tensor* T, Tensor* Y) override
    {
        // Formula
        // -SUM( p(x) * log(q(x)) )
        tensor y_true = T->clone();
        tensor y_pred = Y->clone();
        y_true->toCPU();
        y_pred->toCPU();

        float sum = 0.0f;
        constexpr float eps = FLT_EPSILON;

        if (weights_.size() != y_true->shape[1]) {
            throw std::runtime_error("weights must have same shape of pred->shape[1]");
        }

    #pragma omp parallel for reduction(+:sum)
        for (int bi = 0; bi < y_true->shape[0]; bi++) {  // Batches
            int step_i = bi * y_true->stride[0];

            // Compute cross-entropy
            float bi_sum = 0.0f;
            for (int i = 0; i < y_true->shape[1]; i++) {
                bi_sum += weights_[i] * (y_true->ptr[step_i + i] * std::log(y_pred->ptr[step_i + i] + eps));
                //bi_sum += y_true->ptr[step_i + i] * std::log(y_pred->ptr[step_i + i] + eps);
            }
            sum += bi_sum;
        }

        // Compute mean
        float mean_ce = -sum;//(float)y_true->shape[0];
        return mean_ce;
    }
    Loss* clone() override
    {
        return new LWCategoricalCrossEntropy(weights_);
    }
};

// weights = median(dataset_freq) / dataset_freq
vector<float> get_weights()
{
    //vector<float>train_freq{ 3322, 10175, 2423, 627, 2024, 149, 163, 448 }; // freqs of skin lesion training
    vector<float>dataset_freq{ 4522, 12875, 3323, 867, 2624, 239, 253, 628 }; // freqs of skin lesion dataset
    float median = 0.5f * (dataset_freq[dataset_freq.size() / 2 - 1] + dataset_freq[dataset_freq.size() / 2]);
    std::for_each(dataset_freq.begin(), dataset_freq.end(), [&median](float& n) { n = median / n; });
    return dataset_freq;
}

int main(int argc, char* argv[])
{
    // Download pretrained resnet18
    system("curl -O -J -L  https://drive.google.com/uc?id=1-ZoqAQm8Ie_gHc1ozs6bbBOWVCZFvJ8x&export=download");
    
    // Settings
    Settings s(8, { 224,224 }, "ResNet50", "sce", 1e-5f, "skin_lesion_classification");
    if (!TrainingOptions(argc, argv, s)) {
        return EXIT_FAILURE;
    }
    int workers = 6;
    int frozen_epochs = 10;
    bool freeze = true;

    // onnx resnet generated by pytorch
    removeLayer(s.net, "Gemm_68"); // remove last Linear of resnet18
    //removeLayer(s.net, "resnetv15_dense0_fwd"); // remove last Linear of resnet18-v1-7
    //removeLayer(s.net, "Gemm_174"); // remove last Linear of resnet50

    auto top = getLayer(s.net, "Flatten_67"); // get flatten of resnet18
    //auto top = getLayer(s.net, "flatten_170"); // get flatten of resnet18-v1-7
    //auto top = getLayer(s.net, "Flatten_173"); // get flatten of resnet50

    layer out = Softmax(Dense(top, s.num_classes, true, "classifier")); // true is for the bias.
    auto data_input = getLayer(s.net, "input");
    //auto data_input = getLayer(s.net, "data"); // input of resnet18-v1-7
    s.net = Model({ data_input }, { out });

    Loss* loss_fn = new LCategoricalCrossEntropy();
    //    Loss* loss_fn = new LWCategoricalCrossEntropy(get_weights());

        // Build model
    s.net->build(
        adam(s.lr),
        { loss_fn },
        { getMetric("accuracy") },
        s.cs,
        s.random_weights,
        true, true);

    initializeLayer(s.net, "classifier");
    for (auto l : s.net->layers) {
        if (l->name != "classifier")
            setTrainable(s.net, l->name, false);
    }

    // View model
    summary(s.net);
    setlogfile(s.net, s.exp_name);

    auto training_augs = make_shared<SequentialAugmentationContainer>(
        AugResizeDim(s.size, InterpolationType::cubic),
        AugMirror(.5),
        AugFlip(.5),
        AugRotate({ -180, 180 }),
        AugAdditivePoissonNoise({ 0, 10 }),
        AugGammaContrast({ .5, 1.5 }),
        AugGaussianBlur({ .0, .8 }),
        AugCoarseDropout({ 0, 0.03 }, { 0, 0.05 }, 0.25),
        AugNormalize({ 0.6681, 0.5301, 0.5247 }, { 0.1337, 0.1480, 0.1595 })
        );

    auto validation_augs = make_shared<SequentialAugmentationContainer>(AugResizeDim(s.size, InterpolationType::cubic));

    DatasetAugmentations dataset_augmentations{ { training_augs, validation_augs, nullptr } };

    // Read the dataset
    cout << "Reading dataset" << endl;
    DLDataset d(s.dataset_path, s.batch_size, dataset_augmentations, ecvl::ColorType::RGB);
    // Create producer thread with 'DLDataset d' and 'std::queue q'
    int num_samples = vsize(d.GetSplit());
    int num_batches = num_samples / s.batch_size;
    DataGenerator d_generator_t(&d, s.batch_size, s.size, { vsize(d.classes_) }, workers);

    d.SetSplit(SplitType::validation);
    int num_samples_validation = vsize(d.GetSplit());
    int num_batches_validation = num_samples_validation / s.batch_size;
    DataGenerator d_generator_v(&d, s.batch_size, s.size, { vsize(d.classes_) }, workers);

    tensor output, target, result, single_image;
    float sum = 0.f, ca = 0.f, best_metric = 0.f, mean_metric;

    vector<float> total_metric;
    Metric* metric_fn = getMetric("accuracy");
    View<DataType::float32> img_t;
    ofstream of;
    mt19937 g(random_device{}());

    vector<int> indices(s.batch_size);
    iota(indices.begin(), indices.end(), 0);
    cv::TickMeter tm;
    cv::TickMeter tm_epoch;

    cout << "Starting training" << endl;
    for (int i = 0; i < s.epochs; ++i) {
        tm_epoch.reset();
        tm_epoch.start();

        if (freeze && i >= frozen_epochs) {
            freeze = false;
            for (auto l : s.net->layers) {
                setTrainable(s.net, l->name, true);
            }
        }

        auto current_path{ s.result_dir / path("Epoch_" + to_string(i)) };
        if (s.save_images) {
            for (const auto& c : d.classes_) {
                create_directories(current_path / path(c));
            }
        }

        d.SetSplit(SplitType::training);
        // Reset errors
        reset_loss(s.net); // for train_batch
        total_metric.clear();

        // Shuffle training list
        shuffle(std::begin(d.GetSplit()), std::end(d.GetSplit()), g);
        d.ResetAllBatches();

        d_generator_t.Start();
        set_mode(s.net, 1);
        // Feed batches to the model
        for (int j = 0; d_generator_t.HasNext() /* j < num_batches */; ++j) {
            tm.reset();
            tm.start();
            cout << "Epoch " << i << "/" << s.epochs - 1 << " (batch " << j << "/" << num_batches - 1 << ") - ";
            cout << "|fifo| " << d_generator_t.Size() << " - ";

            tensor x, y;
            // Load a batch
            if (d_generator_t.PopBatch(x, y)) {
                // Preprocessing
                x->div_(255.0);

                // Train batch
                train_batch(s.net, { x }, { y }, indices);

                // Print errors
                print_loss(s.net, j);

                delete x;
                delete y;
            }
            tm.stop();
            cout << "Elapsed time: " << tm.getTimeSec() << endl;
        }
        d_generator_t.Stop();
        tm_epoch.stop();
        cout << "Epoch elapsed time: " << tm_epoch.getTimeSec() << endl;

        // Validation
        d.SetSplit(SplitType::validation);
        d_generator_v.Start();
        set_mode(s.net, 0);
        cout << "Starting validation:" << endl;
        for (int j = 0, n = 0; d_generator_v.HasNext(); ++j) {
            cout << "Validation: Epoch " << i << "/" << s.epochs - 1 << " (batch " << j << "/" << num_batches_validation - 1
                << ") - ";
            tensor x, y;
            // Load a batch
            if (d_generator_v.PopBatch(x, y)) {
                // Preprocessing
                x->div_(255.0);

                // Evaluate batch
                forward(s.net, { x }); // forward does not require reset_loss
                output = getOutput(out);
                ca = metric_fn->value(y, output);

                total_metric.push_back(ca);
                if (s.save_images) {
                    for (int k = 0; k < s.batch_size; ++k, ++n) {
                        result = output->select({ to_string(k) });
                        target = y->select({ to_string(k) });
                        //result->toGPU();
                        //target->toGPU();
                        float max = std::numeric_limits<float>::min();
                        int classe = -1;
                        int gt_class = -1;
                        for (unsigned c = 0; c < result->size; ++c) {
                            if (result->ptr[c] > max) {
                                max = result->ptr[c];
                                classe = c;
                            }

                            if (target->ptr[c] == 1.) {
                                gt_class = c;
                            }
                        }

                        single_image = x->select({ to_string(k) });
                        TensorToView(single_image, img_t);
                        img_t.colortype_ = ColorType::BGR;
                        single_image->mult_(255.);

                        path filename = d.samples_[d.GetSplit()[n]].location_[0].filename();

                        path cur_path = current_path / d.classes_[classe] /
                            filename.replace_extension("_gt_class_" + to_string(gt_class) + ".png");
                        ImWrite(cur_path, img_t);
                        delete single_image;
                        delete result;
                        delete target;
                    }
                }
                cout << " categorical_accuracy: " << ca / s.batch_size << endl;

                delete output;
                delete x;
                delete y;
            }
        }
        d_generator_v.Stop();

        mean_metric = accumulate(total_metric.begin(), total_metric.end(), 0.0f) / (total_metric.size() * s.batch_size);
        cout << "Validation categorical accuracy: " << mean_metric << endl;

        if (mean_metric > best_metric) {
            cout << "Saving weights..." << endl;
            save_net_to_onnx_file(s.net, (s.checkpoint_dir / path(s.exp_name + "_epoch_" + to_string(i) + ".onnx")).string());
            best_metric = mean_metric;
        }

        of.open(s.exp_name + "_stats.txt", ios::out | ios::app);
        of << "Epoch " << i << " - Total categorical accuracy: " << mean_metric << endl;
        of.close();
    }

    return EXIT_SUCCESS;
}