#include "../../lib/system_clock_time_point_string_conversion/system_clock_time_point_string_conversion.h"

#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include <opencv2/imgcodecs/imgcodecs.hpp>
#include <opencv2/imgproc/imgproc.hpp> // cv::putText
#include <opencv2/highgui/highgui.hpp>

int main(int argc, char* argv[])
{   
    numcfc::IniFile iniFile("ImageViewer.ini");

    claim::PostOffice postOffice;
    postOffice.Initialize(iniFile, "IV");
    postOffice.Subscribe("Image");

    if (iniFile.IsDirty()) {
        iniFile.Save();
    }

    while (true) {
        slaim::Message msgLastReceived;

        double timeout_s = 1.0;
        while (postOffice.Receive(msgLastReceived, timeout_s)) {
            timeout_s = 0.0;
        }

        if (msgLastReceived.m_type == "Image") {
            const auto now = std::chrono::system_clock::now();
            claim::AttributeMessage amsg(msgLastReceived);
            const auto& data = amsg.m_attributes["data"];
            if (!data.empty()) {
                const std::vector<unsigned char> buffer(data.begin(), data.end());
                cv::Mat mat = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
                if (!mat.empty()) {
                    auto text = amsg.m_attributes["id"];
                    const auto& timestamp = amsg.m_attributes["timestamp"];
                    if (!timestamp.empty()) {
                        const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - system_clock_time_point_string_conversion::from_string(timestamp)
                        );
                        text += " - delay = " + std::to_string(delay.count()) + " ms";
                    }
                    const auto putText = [&mat, &text](cv::Scalar& color, int thickness) {
                        const cv::Point textOrigin(10, 20);
                        cv::putText(mat, text, textOrigin, cv::FONT_HERSHEY_PLAIN, 1.0, color, thickness);
                    };
                    putText(cv::Scalar(0, 0, 0),       3);
                    putText(cv::Scalar(255, 255, 255), 1);
                    cv::imshow("Image", mat);
                    if (27 == cv::waitKey(1)) {
                        std::cout << std::endl << "Esc pressed" << std::endl;
                        break;
                    }
                }
            }
        }
    }
}
