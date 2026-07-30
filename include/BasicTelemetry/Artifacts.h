#pragma once

#include <BasicTelemetry/Telemetry.h>

#include <filesystem>
#include <memory>
#include <string>

namespace basic_telemetry
{
struct ArtifactOptions
{
    std::filesystem::path outputDirectory;
    bool writeSqlite{ true };
    bool writeMarkdown{ true };
};

[[nodiscard]] std::shared_ptr<Sink> MakeArtifactSink(ArtifactOptions options);
[[nodiscard]] bool WriteArtifacts(
    const SessionSnapshot& snapshot,
    const ArtifactOptions& options,
    std::string* error = nullptr);
}
