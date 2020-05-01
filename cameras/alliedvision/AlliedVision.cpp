#include <VimbaCPP/Include/VimbaCPP.h>

#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include "../../lib/system_clock_time_point_string_conversion/system_clock_time_point_string_conversion.h"

#include "../../lib/tuc/include/tuc/string.hpp"
#include "../../lib/tuc/include/tuc/to_string.hpp"

#include "../../lib/shared_buffer/shared_buffer.h"

#include <opencv2/imgcodecs/imgcodecs.hpp>
#include <opencv2/imgproc/imgproc.hpp> // required at least for Bayer conversion

#include <unordered_map>
#include <iomanip>
#include <deque>

#define CHECK_VIMBA(call) {                                                                \
    const auto result = call;                                                              \
    if (result != VmbErrorSuccess) {                                                       \
        throw std::runtime_error("Error " + std::to_string(result) + " calling " + #call); \
    }                                                                                      \
}

namespace {
    bool isRunning = true;
}

BOOL WINAPI consoleCtrlHandler(_In_ DWORD dwCtrlType)
{
    std::string eventDescription;

    switch (dwCtrlType)
    {
    default:
        numcfc::Logger::LogAndEcho("Control event " + std::to_string(dwCtrlType), "log_control_events");
        return FALSE; // control signal not really handled

    case CTRL_C_EVENT:        eventDescription = "Ctrl-C event";     break;
    case CTRL_BREAK_EVENT:    eventDescription = "Ctrl-Break event"; break;
    case CTRL_CLOSE_EVENT:    eventDescription = "Close event";      break;
    case CTRL_LOGOFF_EVENT:   eventDescription = "Logoff event";     break;
    case CTRL_SHUTDOWN_EVENT: eventDescription = "Shutdown event";   break;
    }

    numcfc::Logger::LogAndEcho(eventDescription, "log_control_events");

    isRunning = false;
    return TRUE;
}

void LogVimbaVersion(AVT::VmbAPI::VimbaSystem& vimbaSystem)
{
    VmbVersionInfo_t versionInfo;
    CHECK_VIMBA(vimbaSystem.QueryVersion(versionInfo));

    numcfc::Logger::LogAndEcho("Vimba version: "
        + std::to_string(versionInfo.major) + "."
        + std::to_string(versionInfo.minor) + "."
        + std::to_string(versionInfo.patch), "log_vimba_version");
}

struct ImageEncodingInputItem {
    cv::Mat rawData;
    VmbPixelFormatType pixelFormat = static_cast<VmbPixelFormatType>(0);
    std::chrono::system_clock::time_point timestamp;
    uint64_t counter = std::numeric_limits<uint64_t>::max();
};

class FrameObserver : public AVT::VmbAPI::IFrameObserver {
public: 
    FrameObserver(AVT::VmbAPI::CameraPtr camera, shared_buffer<ImageEncodingInputItem>& imageEncodingInput)
        : AVT::VmbAPI::IFrameObserver( camera )
        , camera(camera)
        , imageEncodingInput(imageEncodingInput)
    {
        CHECK_VIMBA(camera->GetFeatureByName("DeviceTemperature", temperatureFeature));
    }

    void FrameReceived(const AVT::VmbAPI::FramePtr frame) {
        const auto timestamp = std::chrono::system_clock::now();
        VmbFrameStatusType frameStatus;
        const auto res = frame->GetReceiveStatus(frameStatus);
        if (VmbErrorSuccess == res) {
            if (VmbFrameStatusComplete == frameStatus) {
                // Frame receivd successfully
                VmbUint32_t width = 0, height = 0;
                VmbUchar_t* data = 0;
                VmbPixelFormatType pixelFormat = static_cast<VmbPixelFormatType>(0);
                CHECK_VIMBA(frame->GetWidth(width));
                CHECK_VIMBA(frame->GetHeight(height));
                CHECK_VIMBA(frame->GetBuffer(data));
                CHECK_VIMBA(frame->GetPixelFormat(pixelFormat));

                ImageEncodingInputItem imageEncodingInputItem;

                const cv::Mat temp(height, width, CV_8UC1, data);
                temp.copyTo(imageEncodingInputItem.rawData);

                imageEncodingInputItem.pixelFormat = pixelFormat;
                imageEncodingInputItem.timestamp = timestamp;
                imageEncodingInputItem.counter = counter;

                imageEncodingInput.push_back(std::move(imageEncodingInputItem));

                LogCompleteFramesPerSecond();
            }
            else if (VmbFrameStatusIncomplete == frameStatus) {
                LogIncompleteFramesPerSecond();
            }
            else if (VmbFrameStatusTooSmall == frameStatus) {
                numcfc::Logger::LogAndEcho("Frame buffer too small", "log_errors");
            }
            else if (VmbFrameStatusInvalid == frameStatus) {
                numcfc::Logger::LogAndEcho("Frame buffer not valid", "log_errors");
            }
            else {
                numcfc::Logger::LogAndEcho("Unexpected frame status: " + std::to_string(frameStatus), "log_errors");
            }
        }
        else {
            numcfc::Logger::LogAndEcho("Error getting receive status: " + std::to_string(res), "log_errors");
        }

        ++counter;

        camera->QueueFrame(frame);
    }

private:
    static void UpdateFramesPerSecond(std::deque<std::chrono::steady_clock::time_point>& recentTimestamps, const std::chrono::steady_clock::time_point& now) {
        recentTimestamps.push_back(now);
        while (recentTimestamps.size() > 2 && now - recentTimestamps.front() > std::chrono::milliseconds(1000)) {
            recentTimestamps.pop_front();
        }
    }

    static double GetFramesPerSecond(const std::deque<std::chrono::steady_clock::time_point>& recentTimestamps) {
        if (recentTimestamps.size() > 1) {
            const auto period = recentTimestamps.back() - recentTimestamps.front();
            const auto period_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
            const double period_s = period_ns * 1e-9;
            const double fps = (recentTimestamps.size() - 1) / period_s;
            return fps;
        }
        else {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }

    void LogCompleteFramesPerSecond() {
        const auto now = std::chrono::steady_clock::now();

        UpdateFramesPerSecond(recentCompleteFrameTimestamps, now);

        if (recentCompleteFrameTimestamps.size() > 1) {
            if (now >= fpsCompleteFramesNextLog) {
                const double fps = GetFramesPerSecond(recentCompleteFrameTimestamps);
                const double temperature = GetCameraTemperature();

                std::ostringstream logEntry;
                logEntry << "FPS: " << tuc::to_string(fps, 6) << ", temperature: " << std::fixed << std::setprecision(2) << temperature;
                numcfc::Logger::LogAndEcho(logEntry.str(), "log_fps");

                fpsCompleteFramesNextLog += std::chrono::milliseconds(1000);
            }
        }
        else {
            numcfc::Logger::LogAndEcho("First frame received", "log_fps");
            fpsCompleteFramesNextLog = now + std::chrono::milliseconds(1000);
        }
    }

    void LogIncompleteFramesPerSecond() {
        const auto now = std::chrono::steady_clock::now();

        UpdateFramesPerSecond(recentIncompleteFrameTimestamps, now);

        if (recentIncompleteFrameTimestamps.size() > 1) {
            if (now >= fpsIncompleteFramesNextLog) {
                const double fps = GetFramesPerSecond(recentIncompleteFrameTimestamps);
                numcfc::Logger::LogAndEcho("Incomplete frames per second: " + tuc::to_string(fps, 6), "log_incomplete_frames");
                fpsIncompleteFramesNextLog += std::chrono::milliseconds(1000);
            }
        }
        else {
            numcfc::Logger::LogAndEcho("First incomplete frame received", "log_incomplete_frames");
            fpsIncompleteFramesNextLog = now + std::chrono::milliseconds(1000);
        }
    }

    double GetCameraTemperature() {
        double temperature = std::numeric_limits<double>::quiet_NaN();
        CHECK_VIMBA(temperatureFeature->GetValue(temperature));
        return temperature;
    }

    AVT::VmbAPI::CameraPtr camera;
    shared_buffer<ImageEncodingInputItem>& imageEncodingInput;
    uint64_t counter = 0;
    std::deque<std::chrono::steady_clock::time_point> recentCompleteFrameTimestamps;
    std::deque<std::chrono::steady_clock::time_point> recentIncompleteFrameTimestamps;
    std::chrono::steady_clock::time_point fpsCompleteFramesNextLog;
    std::chrono::steady_clock::time_point fpsIncompleteFramesNextLog;
    AVT::VmbAPI::FeaturePtr temperatureFeature;
};

int main(int argc, char* argv[])
{
    if (!SetConsoleCtrlHandler(consoleCtrlHandler, TRUE)) {
        std::cerr << "Error calling SetConsoleCtrlHandler" << std::endl;
    }

    while (isRunning) {
        try {
            numcfc::IniFile iniFile("AlliedVision.ini");

            claim::PostOffice postOffice;

            postOffice.Initialize(iniFile, "AV");

            const std::string imageFormat = iniFile.GetSetValue("ImageEncoding", "ImageFormat", "jpg");
            const bool isJpeg = tuc::string::equal_case_insensitive(imageFormat, "jpg") || tuc::string::equal_case_insensitive(imageFormat, "jpeg");

            const double jpegCompressionQuality = isJpeg
                ? iniFile.GetSetValue("ImageEncoding", "JpegCompressionQuality", 90)
                : std::numeric_limits<double>::quiet_NaN();

            const auto defaultImageEncodingThreadCount = std::max(2u, std::thread::hardware_concurrency()) - 1;
            const size_t imageEncodingThreadCount = static_cast<size_t>(iniFile.GetSetValue("ImageEncoding", "ThreadCount", defaultImageEncodingThreadCount));

            const size_t totalFrameBufferCount = static_cast<size_t>(iniFile.GetSetValue("FrameBuffers", "TotalCount", 100));

            const double noImagesTimeout_s = iniFile.GetSetValue("Operation", "NoImagesTimeout_s", 10.0);

            if (iniFile.IsDirty()) {
                iniFile.Save();
            }

            using namespace AVT::VmbAPI;

            auto& vimbaSystem = VimbaSystem::GetInstance();

            LogVimbaVersion(vimbaSystem);

            numcfc::Logger::LogAndEcho("Starting Vimba system...");

            CHECK_VIMBA(vimbaSystem.Startup());

            shared_buffer<ImageEncodingInputItem> imageEncodingInput;

            auto imageLastReceived = std::chrono::steady_clock::now();
            std::mutex imageLastReceivedMutex;

            const auto encodeImages = [&]() {

                std::vector<uchar> encodingBuffer;
                cv::Mat image;

                while (imageEncodingInput.is_enabled()) {
                    ImageEncodingInputItem item;
                    if (imageEncodingInput.pop_front(item, std::chrono::milliseconds(1000))) {
                        switch (item.pixelFormat) {
                            case VmbPixelFormatMono8:
                            {
                                image = item.rawData;
                                break;
                            }
                            case VmbPixelFormatBayerRG8:
                            {
                                cv::cvtColor(item.rawData, image, cv::COLOR_BayerBG2BGR);
                                break;
                            }
                            default:
                            {
                                numcfc::Logger::LogAndEcho("Unsupported pixel format: " + std::to_string(item.pixelFormat), "log_errors");
                                image = item.rawData;
                            }
                        }

                        std::vector<int> encodingParameters;
                        if (!std::isnan(jpegCompressionQuality)) {
                            encodingParameters.push_back(cv::IMWRITE_JPEG_QUALITY);
                            encodingParameters.push_back(static_cast<int>(std::round(jpegCompressionQuality)));
                        }

                        cv::imencode("." + imageFormat, image, encodingBuffer, encodingParameters);

                        const std::string timestamp = system_clock_time_point_string_conversion::to_string(item.timestamp);

                        const auto getId = [](const std::string& timestamp, size_t counter, const std::string& imageFormat) {
                            std::string id = timestamp;
                            std::replace(id.begin(), id.end(), ':', '.');
                            std::ostringstream oss;
                            oss << std::hex << std::setw(16) << std::setfill('0') << counter;
                            id += "_" + oss.str() + "." + imageFormat;
                            return id;
                        };

                        claim::AttributeMessage amsg;
                        amsg.m_type = "Image";
                        amsg.m_attributes["id"] = getId(timestamp, item.counter, imageFormat);
                        amsg.m_attributes["timestamp"] = timestamp;
                        amsg.m_attributes["counter"] = std::to_string(item.counter);
                        amsg.m_attributes["rows"] = std::to_string(image.rows);
                        amsg.m_attributes["cols"] = std::to_string(image.cols);
                        amsg.m_attributes["data"] = std::string(encodingBuffer.begin(), encodingBuffer.end());
                        amsg.m_attributes["format"] = imageFormat;
                        if (!std::isnan(jpegCompressionQuality)) {
                            amsg.m_attributes["jpegQuality"] = std::to_string(jpegCompressionQuality);
                        }
                        postOffice.Send(amsg);

                        if (noImagesTimeout_s > 0) {
                            std::lock_guard<std::mutex> lock(imageLastReceivedMutex);
                            imageLastReceived = std::chrono::steady_clock::now();
                        }
                    }
                }
            };

            std::deque<std::thread> imageEncodingThreads;
            for (size_t i = 0; i < imageEncodingThreadCount; ++i) {
                imageEncodingThreads.emplace_back(encodeImages);
            }

            try {
                numcfc::Logger::LogAndEcho("Vimba system started.");

                CameraPtrVector cameras;

                CHECK_VIMBA(vimbaSystem.GetCameras(cameras));

                if (cameras.empty()) {
                    throw std::runtime_error("No cameras found");
                }

                numcfc::Logger::LogAndEcho("Found " + std::to_string(cameras.size()) + " camera" + (cameras.size() == 1 ? "" : "s") + ":");

                for (const auto& camera : cameras) {
                    std::string model, id;
                    CHECK_VIMBA(camera->GetModel(model));
                    CHECK_VIMBA(camera->GetID(id));
                    numcfc::Logger::LogAndEcho("  " + id + " : " + model);
                }

                std::unordered_map<std::string, FramePtrVector> frames;
                std::unordered_map<std::string, IFrameObserverPtr> frameObservers;

                for (auto& camera : cameras) {
                    CHECK_VIMBA(camera->Open(VmbAccessModeFull));

                    FeaturePtr feature;

                    const auto vimbaParameters = iniFile.GetKeys("VimbaParameters");
                    for (const auto& parameterName : vimbaParameters) {
                        const auto value = iniFile.GetValue("VimbaParameters", parameterName);

                        numcfc::Logger::LogAndEcho(parameterName + " = " + value, "log_camera_parameters");

                        CHECK_VIMBA(camera->GetFeatureByName(parameterName.c_str(), feature));

                        VmbFeatureDataType dataType = VmbFeatureDataUnknown;
                        CHECK_VIMBA(feature->GetDataType(dataType));

                        switch (dataType) {
                        case VmbFeatureDataInt:    CHECK_VIMBA(feature->SetValue(std::stoi(value))); break;
                        case VmbFeatureDataFloat:  CHECK_VIMBA(feature->SetValue(std::stod(value))); break;
                        case VmbFeatureDataEnum:   CHECK_VIMBA(feature->SetValue(value.c_str())); break;
                        case VmbFeatureDataString: CHECK_VIMBA(feature->SetValue(value.c_str())); break;
                        case VmbFeatureDataBool:   CHECK_VIMBA(feature->SetValue(std::stoi(value) != 0)); break;
                        default: throw std::runtime_error("Unsupported data type: " + std::to_string(dataType) + " (parameter name: " + parameterName + ")");
                        }
                    }

                    VmbInt64_t payloadSize = -1;
                    CHECK_VIMBA(camera->GetFeatureByName("PayloadSize", feature));
                    CHECK_VIMBA(feature->GetValue(payloadSize));
                    
                    const auto frameCount = std::max(1ull, totalFrameBufferCount / cameras.size());

                    std::string id;
                    CHECK_VIMBA(camera->GetID(id));

                    numcfc::Logger::LogAndEcho("Camera " + id + ": payload size = " + std::to_string(payloadSize));

                    frameObservers[id].reset(new FrameObserver(camera, imageEncodingInput));

                    frames[id].resize(frameCount);

                    for (auto& frame : frames[id]) {
                        frame.reset(new Frame(payloadSize));
                        CHECK_VIMBA(frame->RegisterObserver(frameObservers[id]));
                        CHECK_VIMBA(camera->AnnounceFrame(frame));
                    }

                    CHECK_VIMBA(camera->StartCapture());

                    for (auto& frame : frames[id]) {
                        camera->QueueFrame(frame);
                    }

                    CHECK_VIMBA(camera->GetFeatureByName("AcquisitionStart", feature));
                    CHECK_VIMBA(feature->RunCommand());
                }
                
                if (iniFile.IsDirty()) {
                    iniFile.Save();
                }
                else {
                    iniFile.Refresh(); // update time-modified information inside the object
                }

                while (isRunning) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));

                    if (iniFile.Refresh()) {
                        numcfc::Logger::LogAndEcho("Ini file updated, starting over...");
                        break;
                    }

                    if (noImagesTimeout_s > 0) {
                        std::lock_guard<std::mutex> lock(imageLastReceivedMutex);
                        const auto durationSinceLatestImageReceived = std::chrono::steady_clock::now() - imageLastReceived;
                        const auto timeoutDuration = std::chrono::milliseconds(static_cast<int>(std::round(noImagesTimeout_s * 1000)));
                        if (durationSinceLatestImageReceived > timeoutDuration) {
                            const auto duration_s = std::chrono::duration_cast<std::chrono::seconds>(durationSinceLatestImageReceived).count();
                            numcfc::Logger::LogAndEcho("No image received in " + std::to_string(duration_s) + " s, starting over...");
                            break;
                        }
                    }
                }
            }
            catch (std::exception& e) {
                numcfc::Logger::LogAndEcho(e.what(), "log_errors");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            imageEncodingInput.halt();

            for (auto& imageEncodingThread : imageEncodingThreads) {
                imageEncodingThread.join();
            }

            CHECK_VIMBA(vimbaSystem.Shutdown());
        }
        catch (std::exception& e) {
            numcfc::Logger::LogAndEcho(e.what(), "log_errors");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
