#include <messaging/claim/PostOffice.h>
#include <messaging/claim/AttributeMessage.h>

#include <numcfc/IniFile.h>
#include <numcfc/Logger.h>

#include "../lib/isto/system_clock_time_point_string_conversion/system_clock_time_point_string_conversion.h"

#include <isto.h>

int main(int argc, char* argv[])
{   
    numcfc::IniFile iniFile("ImageStorage.ini");

    claim::PostOffice postOffice;
    postOffice.Initialize(iniFile, "ISto");
    postOffice.Subscribe("Image");

    isto::Configuration configuration;
    configuration.maxRotatingDataToKeepInGiB = iniFile.GetSetValue("ImageStorage", "MaxRotatingDataToKeep_GiB", configuration.maxRotatingDataToKeepInGiB, "Max rotating data to keep (gibibytes)");
    configuration.minFreeDiskSpaceInGiB = iniFile.GetSetValue("ImageStorage", "MinFreeDiskSpace_GiB", configuration.minFreeDiskSpaceInGiB, "Minimum free disk space (gibibytes)");
    
    const std::string directoryStructureResolution = iniFile.GetSetValue("ImageStorage", "DirectoryStructureResolution", "min", "Directory structure resolution - try \"min\" for minutes, \"h\" for hours, or \"d\" for days");

    if (directoryStructureResolution == "min") {
        configuration.directoryStructureResolution = isto::Configuration::DirectoryStructureResolution::Minutes;
    }
    else if (directoryStructureResolution == "h") {
        configuration.directoryStructureResolution = isto::Configuration::DirectoryStructureResolution::Hours;
    }
    else if (directoryStructureResolution == "d") {
        configuration.directoryStructureResolution = isto::Configuration::DirectoryStructureResolution::Days;
    }
    else {
        numcfc::Logger::LogAndEcho("Unexpected directory structure resolution: " + directoryStructureResolution + " (using minutes)", "log_errors");
        configuration.directoryStructureResolution = isto::Configuration::DirectoryStructureResolution::Minutes;
    }

#ifdef _WIN32
    const auto defaultDataDirectory = ".\\data";
#else
    const auto defaultDataDirectory = "./data";
#endif

    configuration.rotatingDirectory = iniFile.GetSetValue("ImageStorage", "DataDirectory", defaultDataDirectory, "The directory where to store the image data");
    configuration.permanentDirectory = iniFile.GetValue("ImageStorage", "PermanentDataDirectory");

    if (configuration.permanentDirectory.empty()) {
        // this is the main supported mode of operation: keep all data in the same directory (for a better anno experience)
        configuration.permanentDirectory = configuration.rotatingDirectory;
    }

    if (iniFile.IsDirty()) {
        iniFile.Save();
    }

    isto::Storage storage(configuration);

    while (true) {
        slaim::Message msg;
        if (postOffice.Receive(msg, 1.0)) {
            claim::AttributeMessage amsg(msg);
            const auto& data = amsg.m_attributes["data"];
            if (!data.empty()) {
                const auto& format = amsg.m_attributes["format"];
                const auto id = amsg.m_attributes["id"] + "." + format;
                const auto& timestamp = amsg.m_attributes["timestamp"];
                const bool isPermanent = false;
                isto::DataItem dataItem(
                    id,
                    data,
                    timestamp.empty()
                        ? std::chrono::system_clock::now()
                        : system_clock_time_point_string_conversion::from_string(timestamp),
                    isPermanent
                );
                storage.SaveData(dataItem);
            }
        }
    }
}
