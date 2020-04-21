#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include <opencv2/imgcodecs/imgcodecs.hpp>
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
        slaim::Message msg;
        if (postOffice.Receive(msg, 1.0)) {
            claim::AttributeMessage amsg(msg);
            const auto& data = amsg.m_attributes["data"];
            if (!data.empty()) {
                const std::vector<unsigned char> buffer(data.begin(), data.end());
                const cv::Mat mat = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
                if (!mat.empty()) {
                    cv::imshow("Image", mat);
                    cv::waitKey(1);
                }
            }
        }
    }
}
