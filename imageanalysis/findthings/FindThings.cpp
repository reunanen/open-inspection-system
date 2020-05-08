#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include "../../lib/annonet/annonet_things/annonet_infer.h"
#include "../../lib/annonet/annonet_things/annonet_parse_anno_classes.h"

#include "dlib/image_loader/load_image.h"

#include "rapidjson/stringbuffer.h"
#include "rapidjson/prettywriter.h"

inline uint16_t classlabel_to_index_label(const std::string& classlabel, const std::vector<AnnoClass>& anno_classes)
{
    for (const AnnoClass& anno_class : anno_classes) {
        if (anno_class.classlabel == classlabel) {
            return anno_class.index;
        }
    }
    throw std::runtime_error("Unknown class: '" + classlabel + "'");
}

std::string format_anno_results(const std::vector<dlib::mmod_rect>& labels, const std::vector<AnnoClass>& anno_classes)
{
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

    writer.StartArray();

    for (const auto& label : labels) {

        const auto& index = classlabel_to_index_label(label.label, anno_classes);
        const auto& anno_class = anno_classes[index];

        writer.StartObject();
        writer.String("color");
        writer.StartObject();
        {
            writer.String("r"); writer.Int(anno_class.rgba_label.red);
            writer.String("g"); writer.Int(anno_class.rgba_label.green);
            writer.String("b"); writer.Int(anno_class.rgba_label.blue);
            writer.String("a"); writer.Int(anno_class.rgba_label.alpha);
        }
        writer.EndObject();

        writer.String("color_paths");
        writer.StartArray();

        {
            writer.StartArray();
            {
                writer.StartObject();
                writer.String("x"); writer.Int(label.rect.left());
                writer.String("y"); writer.Int(label.rect.top());
                writer.EndObject();
            }
            {
                writer.StartObject();
                writer.String("x"); writer.Int(label.rect.right());
                writer.String("y"); writer.Int(label.rect.top());
                writer.EndObject();
            }
            {
                writer.StartObject();
                writer.String("x"); writer.Int(label.rect.right());
                writer.String("y"); writer.Int(label.rect.bottom());
                writer.EndObject();
            }
            {
                writer.StartObject();
                writer.String("x"); writer.Int(label.rect.left());
                writer.String("y"); writer.Int(label.rect.bottom());
                writer.EndObject();
            }
            writer.EndArray();
        }

        writer.EndArray();
        writer.EndObject();
    }

    writer.EndArray();

    return buffer.GetString();
}

std::vector<double> convert_gains_by_class_to_gains_by_detector_window(const std::vector<double>& gains_by_class, const std::vector<AnnoClass>& anno_classes, const dlib::mmod_options& mmod_options)
{
    DLIB_CASSERT(gains_by_class.size() == anno_classes.size());

    std::vector<double> gains_by_detector_window(mmod_options.detector_windows.size());

    for (size_t detector_window_index = 0, end = gains_by_detector_window.size(); detector_window_index < end; ++detector_window_index) {
        const std::string& classlabel = mmod_options.detector_windows[detector_window_index].label;
        const auto classlabel_index = classlabel_to_index_label(classlabel, anno_classes);
        gains_by_detector_window[detector_window_index] = gains_by_class[classlabel_index];
    }

    return gains_by_detector_window;
}

int main(int argc, char* argv[])
{   
    while (true) {
        try {
            numcfc::IniFile iniFile("FindThings.ini");

            claim::PostOffice postOffice;
            postOffice.Initialize(iniFile, "FT");
            postOffice.Subscribe("Image");

            const std::string modelFilename = iniFile.GetSetValue("AnnonetModel", "Filename", "annonet.dnn");

#if 0
            const int min_input_dimension = NetPimpl::TrainingNet::GetRequiredInputDimension();
#else
            const int min_input_dimension = 16;
#endif

#ifdef DLIB_USE_CUDA
            const auto defaultMaxTileWidth = 4096;
            const auto defaultMaxTileHeight = 4096;
#else
            // in CPU-only mode, we may be able to handle larger tiles
            const auto defaultMaxTileWidth = 4096;
            const auto defaultMaxTileHeight = 4096;
#endif

            tiling::parameters tiling_parameters;
            tiling_parameters.max_tile_width = iniFile.GetSetValue("Tiling", "MaxWidth", defaultMaxTileWidth);
            tiling_parameters.max_tile_height = iniFile.GetSetValue("Tiling", "MaxHeigth", defaultMaxTileHeight);
            tiling_parameters.overlap_x = min_input_dimension;
            tiling_parameters.overlap_y = min_input_dimension;

            DLIB_CASSERT(tiling_parameters.max_tile_width >= min_input_dimension);
            DLIB_CASSERT(tiling_parameters.max_tile_height >= min_input_dimension);

            if (iniFile.IsDirty()) {
                iniFile.Save();
            }

            annonet_infer_temp temp;

            double downscaling_factor = 1.0;
            std::string serialized_runtime_net;
            std::string anno_classes_json;
            dlib::deserialize(modelFilename) >> anno_classes_json >> downscaling_factor >> serialized_runtime_net;

            numcfc::Logger::LogAndEcho("Deserializing annonet, downscaling factor = " + std::to_string(downscaling_factor));

            NetPimpl::RuntimeNet net;
            net.Deserialize(std::istringstream(serialized_runtime_net));

            const std::vector<AnnoClass> anno_classes = parse_anno_classes(anno_classes_json);

            DLIB_CASSERT(anno_classes.size() >= 2);

            const auto get_gains_by_detector_window = [&anno_classes, &net, &iniFile]() {
                std::vector<double> gainsByClass;
                gainsByClass.reserve(anno_classes.size());

                std::ostringstream logEntry;
                logEntry << "Using gains:";

                for (const auto& anno_class : anno_classes) {
                    if (anno_class.classlabel == "<<ignore>>") {
                        gainsByClass.push_back(0);
                    }
                    else {
                        std::string sanitizedClasslabel = anno_class.classlabel;
                        std::replace(sanitizedClasslabel.begin(), sanitizedClasslabel.end(), ' ', '_');
                        const double gain = iniFile.GetSetValue("Gains", sanitizedClasslabel, 0.0);
                        gainsByClass.push_back(gain);
                        logEntry << std::endl << " - " << anno_class.classlabel << ": " << gain;
                    }
                }

                numcfc::Logger::LogAndEcho(logEntry.str());

                return convert_gains_by_class_to_gains_by_detector_window(gainsByClass, anno_classes, net.GetOptions());
            };

            const std::vector<double> gains_by_detector_window = get_gains_by_detector_window();


            if (iniFile.IsDirty()) {
                iniFile.Save();
            }

            iniFile.Refresh();

            NetPimpl::input_type inputImage;
            std::vector<dlib::mmod_rect> labels;

            numcfc::Logger::LogAndEcho("Ready, now waiting for images...");

            bool firstImageReceived = false;

            while (true) {
                if (iniFile.Refresh()) {
                    numcfc::Logger::LogAndEcho("Ini file refreshed, starting over...");
                    break;
                }

                slaim::Message msgLastReceived;

                double timeout_s = 1.0;
                while (postOffice.Receive(msgLastReceived, timeout_s)) {
                    timeout_s = 0.0;
                }

                if (msgLastReceived.m_type == "Image") {
                    claim::AttributeMessage amsg(msgLastReceived);
                    const auto& data = amsg.m_attributes["data"];
                    if (!data.empty()) {
                        const std::string& imageId = amsg.m_attributes["id"];

                        // TODO: avoid going via a file

                        const auto t0 = std::chrono::system_clock::now();
                        {
                            std::ofstream out(imageId, std::ios::binary);
                            out << data;
                        }
                        const auto t1 = std::chrono::system_clock::now();

                        dlib::load_image(inputImage, imageId);

                        std::remove(imageId.c_str());

                        if (!firstImageReceived) {
                            numcfc::Logger::LogAndEcho("First image received, size = " + std::to_string(inputImage.nc()) + " x " + std::to_string(inputImage.nr()) + " (" + std::to_string(data.size()) + " bytes)");
                            firstImageReceived = true;
                        }

                        const auto t2 = std::chrono::system_clock::now();

                        annonet_infer(net, inputImage, labels, gains_by_detector_window, tiling_parameters, temp);

                        const auto t3 = std::chrono::system_clock::now();

                        const auto formatMilliseconds = [](const auto& duration) {
                            return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
                        };

                        numcfc::Logger::LogNoEcho("Analyzed image " + imageId
                            + ": found " + std::to_string(labels.size()) + " things in "
                            + formatMilliseconds(t1 - t0) + " + "
                            + formatMilliseconds(t2 - t1) + " + "
                            + formatMilliseconds(t3 - t2) + " ms", "log_find_things");

                        claim::AttributeMessage amsg;
                        amsg.m_type = "AnnoResultJson";
                        amsg.m_attributes["id"] = imageId + "_result_path.json";
                        amsg.m_attributes["image_id"] = imageId;
                        amsg.m_attributes["data"] = format_anno_results(labels, anno_classes);
                        amsg.m_attributes["timestamp"] = amsg.m_attributes["timestamp"];
                        postOffice.Send(amsg);
                    }
                }
            }
        }
        catch (std::exception& e) {
            numcfc::Logger::LogAndEcho(e.what(), "log_errors");
        }
    }
}
