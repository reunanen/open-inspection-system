#include "../../lib/system_clock_time_point_string_conversion/system_clock_time_point_string_conversion.h"

#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include <opencv2/imgcodecs/imgcodecs.hpp>
#include <opencv2/imgproc/imgproc.hpp> // cv::putText
#include <opencv2/highgui/highgui.hpp>

#include "../../lib/nlohmann_json/single_include/nlohmann/json.hpp"

int main(int argc, char* argv[])
{   
    numcfc::IniFile iniFile("ImageViewer.ini");

    claim::PostOffice postOffice;
    postOffice.Initialize(iniFile, "IV");
    postOffice.Subscribe("Image");
    postOffice.Subscribe("AnnoResultJson");

    if (iniFile.IsDirty()) {
        iniFile.Save();
    }

    std::string currentImageId;
    cv::Mat currentImage;
    std::chrono::system_clock::time_point currentImageReceived;

    const auto putText = [&currentImage](const std::string& text, const cv::Point& origin) {
        cv::putText(currentImage, text, origin, cv::FONT_HERSHEY_PLAIN, 1.0, cv::Scalar(0, 0, 0), 3);
        cv::putText(currentImage, text, origin, cv::FONT_HERSHEY_PLAIN, 1.0, cv::Scalar(255, 255, 255), 1);
    };

    while (true) {
        slaim::Message msg, msgImageLastReceived;

        double timeout_s = 1.0;
        while (postOffice.Receive(msg, timeout_s)) {
            if (msg.GetType() == "Image") {
                msgImageLastReceived = msg;
                timeout_s = 0.0;
            }
            else if (msg.GetType() == "AnnoResultJson") {
                const auto now = std::chrono::system_clock::now();

                claim::AttributeMessage amsg(msg);
                const auto& imageId = amsg.m_attributes["image_id"];

                if (imageId == currentImageId) {
                    const auto json = nlohmann::json::parse(amsg.m_attributes["data"]);

                    for (const auto& classItem : json) {
                        const cv::Scalar color(
                            classItem["color"].value("b", 128),
                            classItem["color"].value("g", 128),
                            classItem["color"].value("r", 255)
                        );

                        std::vector<std::vector<cv::Point>> contours;

                        const auto& colorPaths = classItem["color_paths"];

                        for (const auto& colorPath : colorPaths) {
                            std::vector<cv::Point> contour;

                            for (const auto& point : colorPath) {
                                contour.emplace_back(
                                    point["x"].get<int>(),
                                    point["y"].get<int>()
                                );
                            }
                            contours.push_back(contour);
                        }

                        cv::drawContours(currentImage, contours, -1, color, 1);
                    }

                    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - currentImageReceived
                    );
                    putText("Result receiving delay = " + std::to_string(delay.count()) + " ms", cv::Point(10, 60));

                    cv::imshow("Image", currentImage);
                    if (27 == cv::waitKey(1)) {
                        std::cout << std::endl << "Esc pressed" << std::endl;
                        return 0;
                    }
                }
                else {
                    numcfc::Logger::LogAndEcho("Received result for non-current image " + imageId + ", current = " + currentImageId);
                }
            }
        }

        if (msgImageLastReceived.m_type == "Image") {
            const auto now = std::chrono::system_clock::now();
            claim::AttributeMessage amsg(msgImageLastReceived);
            const auto& data = amsg.m_attributes["data"];
            if (!data.empty()) {
                const std::vector<unsigned char> buffer(data.begin(), data.end());
                currentImage = cv::imdecode(buffer, cv::IMREAD_COLOR);
                if (!currentImage.empty()) {
                    currentImageId = amsg.m_attributes["id"];
                    currentImageReceived = now;
                    putText(currentImageId, cv::Point(10, 20));
                    const auto& timestamp = amsg.m_attributes["timestamp"];
                    if (!timestamp.empty()) {
                        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - system_clock_time_point_string_conversion::from_string(timestamp)
                        );
                        putText("Image receiving delay = " + std::to_string(delay.count()) + " ms", cv::Point(10, 40));
                    }
                    cv::imshow("Image", currentImage);
                    if (27 == cv::waitKey(1)) {
                        std::cout << std::endl << "Esc pressed" << std::endl;
                        return 0;
                    }
                }
            }
        }
        else {
            if (27 == cv::waitKey(1)) {
                std::cout << std::endl << "Esc pressed" << std::endl;
                return 0;
            }
        }
    }
}
