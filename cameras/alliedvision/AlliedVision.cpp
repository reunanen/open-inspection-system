#include <VimbaCPP/Include/VimbaCPP.h>

#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include "../../lib/system_clock_time_point_string_conversion/system_clock_time_point_string_conversion.h"

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

class FrameObserver : public AVT::VmbAPI::IFrameObserver {
public: 
    FrameObserver(AVT::VmbAPI::CameraPtr camera, slaim::PostOffice& postOffice)
        : AVT::VmbAPI::IFrameObserver( camera )
        , camera(camera)
        , postOffice(postOffice)
    {
    }

    void FrameReceived(const AVT::VmbAPI::FramePtr frame) {
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

                cv::Mat mat;

                switch (pixelFormat) {
                    case VmbPixelFormatMono8:
                    {
                        mat = cv::Mat(height, width, CV_8UC1, data);
                        break;
                    }
                    case VmbPixelFormatBayerRG8:
                    {
                        cv::Mat temp(height, width, CV_8UC1, data);
                        cv::cvtColor(temp, mat, cv::COLOR_BayerBG2BGR);
                        break;
                    }
                    default:
                    {
                        numcfc::Logger::LogAndEcho("Unsupported pixel format: " + std::to_string(pixelFormat), "log_errors");
                    }
                }

                std::vector<int> encodingParameters; // TODO: add jpg quality parameter

                const std::string imageFormat = "jpg"; // TODO: make this configurable

                cv::imencode("." + imageFormat, mat, encodingBuffer, encodingParameters);

                const std::string timestamp = system_clock_time_point_string_conversion::to_string(std::chrono::system_clock::now());

                const auto getId = [](const std::string& timestamp, size_t counter) {
                    std::string id = timestamp;
                    std::replace(id.begin(), id.end(), ':', '.');
                    std::ostringstream oss;
                    oss << std::hex << std::setw(16) << std::setfill('0') << counter;
                    id += "_" + oss.str();
                    return id;
                };

                claim::AttributeMessage amsg;
                amsg.m_type = "Image";
                amsg.m_attributes["id"] = getId(timestamp, counter);
                amsg.m_attributes["timestamp"] = timestamp;
                amsg.m_attributes["counter"] = std::to_string(counter);
                amsg.m_attributes["rows"] = std::to_string(mat.rows);
                amsg.m_attributes["cols"] = std::to_string(mat.cols);
                amsg.m_attributes["data"] = std::string(encodingBuffer.begin(), encodingBuffer.end());
                amsg.m_attributes["format"] = imageFormat;
                postOffice.Send(amsg);

                ++counter;

                LogFramesPerSecond();
            }
            else {
                // Frame received unsuccessfully
                numcfc::Logger::LogAndEcho("Frame not received successfully", "log_errors");
            }
        }
        else {
            numcfc::Logger::LogAndEcho("Error calling GetReceiveStatus", "log_errors");
        }

        camera->QueueFrame(frame);
    }

private:
    void LogFramesPerSecond() {
        const auto now = std::chrono::steady_clock::now();
        recentTimestamps.push_back(now);
        while (recentTimestamps.size() > 2 && now - recentTimestamps.front() < std::chrono::milliseconds(1000)) {
            recentTimestamps.pop_front();
        }
        if (recentTimestamps.size() > 1) {
            if (now - fpsLastLogged > std::chrono::milliseconds(1000)) {
                const double period_s = std::chrono::duration_cast<std::chrono::milliseconds>(now - recentTimestamps.front()).count() / 1000.0;
                const double fps = (recentTimestamps.size() - 1) / period_s;
                std::ostringstream logEntry;
                logEntry << "FPS: " << fps;
                numcfc::Logger::LogAndEcho(logEntry.str(), "log_fps");
                fpsLastLogged = now;
            }
        }
        else {
            numcfc::Logger::LogAndEcho("First frame received", "log_fps");
        }
    }

    AVT::VmbAPI::CameraPtr camera;
    slaim::PostOffice& postOffice;
    std::vector<uchar> encodingBuffer;
    uint64_t counter = 0;
    std::deque<std::chrono::steady_clock::time_point> recentTimestamps;
    std::chrono::steady_clock::time_point fpsLastLogged;
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

            if (iniFile.IsDirty()) {
                iniFile.Save();
            }

            using namespace AVT::VmbAPI;

            auto& vimbaSystem = VimbaSystem::GetInstance();

            LogVimbaVersion(vimbaSystem);

            numcfc::Logger::LogAndEcho("Starting Vimba system...");

            CHECK_VIMBA(vimbaSystem.Startup());

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

                const size_t totalFrameBufferCount = 10;
                std::unordered_map<std::string, FramePtrVector> frames;
                std::unordered_map<std::string, IFrameObserverPtr> frameObservers;

                for (auto& camera : cameras) {
                    CHECK_VIMBA(camera->Open(VmbAccessModeFull));
                
                    // TODO: set camera parameters

                    FeaturePtr feature;
                    VmbInt64_t payloadSize = -1;
                    CHECK_VIMBA(camera->GetFeatureByName("PayloadSize", feature));
                    CHECK_VIMBA(feature->GetValue(payloadSize));
                    
                    const auto frameCount = std::max(1ull, totalFrameBufferCount / cameras.size());

                    std::string id;
                    CHECK_VIMBA(camera->GetID(id));

                    numcfc::Logger::LogAndEcho("Camera " + id + ": payload size = " + std::to_string(payloadSize));

                    frameObservers[id].reset(new FrameObserver(camera, postOffice));

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

                while (isRunning) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            catch (std::exception& e) {
                numcfc::Logger::LogAndEcho(e.what(), "log_errors");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                CHECK_VIMBA(vimbaSystem.Shutdown());
            }
        }
        catch (std::exception& e) {
            numcfc::Logger::LogAndEcho(e.what(), "log_errors");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
