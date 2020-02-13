#include "ecvl/core.h"
#include "ecvl/support_eddl.h"
#include "ecvl/dataset_parser.h"
#include "models/models.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>

using namespace ecvl;
using namespace eddl;
using namespace std;

int main()
{
    // Settings
    int epochs = 50;
    int batch_size = 12;
    int num_classes = 8;
    std::vector<int> size{ 224,224 }; // Size of images

    std::mt19937 g(std::random_device{}());

    // Define network
    layer in = Input({ 3, size[0],  size[1] });
    layer out = VGG16(in, num_classes);
    model net = Model({ in }, { out });

    // Build model
    build(net,
        sgd(0.001, 0.9), // Optimizer
        { "soft_cross_entropy" }, // Losses
        { "categorical_accuracy" } // Metrics
    );

    toGPU(net);

    // View model
    summary(net);
    plot(net, "model.pdf");

    auto training_augs = make_unique<SequentialAugmentationContainer>(
        AugMirror(.5),
        AugFlip(.5),
        AugRotate({-180, 180}),
        AugAdditivePoissonNoise({0, 10}),
        AugGammaContrast({.5,1.5}),
        AugGaussianBlur({.0,.8}),
        AugCoarseDropout({0, 0.3}, {0.02, 0.05}, 0.5),
        AugResizeDim(size));

    auto test_augs = make_unique<SequentialAugmentationContainer>(AugResizeDim(size));

    DatasetAugmentations dataset_augmentations{{move(training_augs), nullptr, move(test_augs)}};

    // Read the dataset
    cout << "Reading dataset" << endl;
    DLDataset d("D:/dataset/isic_classification/isic_classification.yml", batch_size, move(dataset_augmentations));

    // Prepare tensors which store batch
    tensor x = eddlT::create({ batch_size, d.n_channels_, size[0], size[1] });
    tensor y = eddlT::create({ batch_size, static_cast<int>(d.classes_.size()) });
    tensor output;
    tensor target;
    tensor result;

    int num_samples = d.GetSplit().size();
    int num_batches = num_samples / batch_size;

    d.SetSplit(SplitType::validation);
    int num_samples_validation = d.GetSplit().size();
    int num_batches_validation = num_samples_validation / batch_size;
    float sum = 0., ca = 0.;

    vector<float> total_metric;
    Metric *m = getMetric("categorical_accuracy");
    float total_avg;
    ofstream of;

    vector<int> indices(batch_size);
    iota(indices.begin(), indices.end(), 0);
    cv::TickMeter tm;

    cout << "Starting training" << endl;
    for (int i = 0; i < epochs; ++i) {
        d.SetSplit(SplitType::training);
        // Reset errors
        reset_loss(net);
        total_metric.clear();

        // Shuffle training list
        shuffle(std::begin(d.GetSplit()), std::end(d.GetSplit()), g);
        d.ResetAllBatches();

        // Feed batches to the model
        for (int j = 0; j < num_batches; ++j) {
            tm.reset();
            tm.start();
            cout << "Epoch " << i << "/" << epochs << " (batch " << j << "/" << num_batches << ") - ";

            // Load a batch
            d.LoadBatch(x, y);

            // Preprocessing
            x->div_(255.0);

            // Prepare data
            vtensor tx{ x };
            vtensor ty{ y };

            // Train batch
            train_batch(net, tx, ty, indices);

            // Print errors
            print_loss(net, j);
            tm.stop();

            cout << "- Elapsed time: " << tm.getTimeSec() << endl;
        }

        cout << "Saving weights..." << endl;
        save(net, "isic_classification_checkpoint_epoch_" + to_string(i) + ".bin", "bin");

        // Evaluation
        d.SetSplit(SplitType::test);

        cout << "Evaluate:" << endl;
        for (int j = 0; j < num_batches_validation; ++j) {
            cout << "Validation: Epoch " << i << "/" << epochs << " (batch " << j << "/" << num_batches_validation << ") - ";

            // Load a batch
            d.LoadBatch(x, y);

            // Preprocessing
            x->div_(255.0);

            // Evaluate batch
            forward(net, { x });
            output = getTensor(out);

            sum = 0.;
            for (int k = 0; k < batch_size; ++k) {
                result = eddlT::select(output, k);
                target = eddlT::select(y, k);

                ca = m->value(target, result);

                total_metric.push_back(ca);
                sum += ca;

                delete result;
                delete target;
            }
            cout << " categorical_accuracy: " << sum / batch_size << endl;
        }

        total_avg = accumulate(total_metric.begin(), total_metric.end(), 0.0) / total_metric.size();
        cout << "Total categorical accuracy: " << total_avg << endl;

        of.open("output_evaluate_classification.txt", ios::out | ios::app);
        of << "Epoch " << i << " - Total categorical accuracy: " << total_avg << endl;
        of.close();
    }

    delete x;
    delete y;
    delete output;

    return EXIT_SUCCESS;
}