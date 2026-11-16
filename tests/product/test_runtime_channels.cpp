// SPDX-License-Identifier: Apache-2.0
// Exercise the installed FreeRDP addins without a display, credentials or network.
#include "core.h"
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/settings.h>
#include <cstdio>
#include <memory>
#include <vector>

static bool checkChannels(const QString &mode, bool sound, bool dynamic)
{
    RDP_CLIENT_ENTRY_POINTS entry{};
    entry.Size = sizeof(entry);
    entry.Version = RDP_CLIENT_INTERFACE_VERSION;
    entry.ContextSize = sizeof(rdpClientContext);
    std::unique_ptr<rdpContext, decltype(&freerdp_client_context_free)> context(
        freerdp_client_context_new(&entry), freerdp_client_context_free);
    if (!context) {
        std::fprintf(stderr, "Failed to create the FreeRDP context\n");
        return false;
    }
    // Satisfy /from-stdin:force without reading stdin or using real credentials.
    if (!freerdp_settings_set_string(context->settings, FreeRDP_Password, "fixture"))
        return false;

    WayWin::Host host;
    host.name = "Channel test";
    host.server = "fixture.invalid";
    host.user = "fixture";
    host.mode = mode;
    host.sound = sound;
    host.dynamic = dynamic;
    auto args = WayWin::arguments(host, false);
    args.prepend("runtime-channel-tests");
    std::vector<QByteArray> storage;
    for (const auto &arg : args)
        storage.push_back(arg.toUtf8());
    std::vector<char *> argv;
    for (auto &arg : storage)
        argv.push_back(arg.data());
    const int status = freerdp_client_settings_parse_command_line_arguments(
        context->settings, int(argv.size()), argv.data(), FALSE);
    if (status != 0) {
        std::fprintf(stderr, "mode=%s sound=%d dynamic=%d argument error=%d\n",
                     qPrintable(mode), sound, dynamic, status);
        return false;
    }

    const bool loaded = freerdp_client_load_addins(context->channels, context->settings);
    std::fprintf(stderr, "mode=%s sound=%d dynamic=%d channels=%s\n",
                 qPrintable(mode), sound, dynamic, loaded ? "loaded" : "failed");
    if (!loaded)
        return false;

    // The core rdpdr channel is needed by RDP8/audio, with no shared devices.
    if (freerdp_settings_get_uint32(context->settings, FreeRDP_DeviceCount) != 0)
        return false;
    for (const auto setting : {FreeRDP_RedirectDrives, FreeRDP_RedirectHomeDrive,
                               FreeRDP_RedirectPrinters, FreeRDP_RedirectSmartCards,
                               FreeRDP_RedirectSerialPorts, FreeRDP_RedirectParallelPorts,
                               FreeRDP_RedirectClipboard, FreeRDP_AudioCapture}) {
        if (freerdp_settings_get_bool(context->settings, setting))
            return false;
    }
    return true;
}

int main()
{
    if (freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0) != 0)
        return 1;
    bool passed = true;
    for (const auto &mode : {QString("desktop"), QString("rail")}) {
        for (bool sound : {false, true}) {
            for (bool dynamic : {false, true})
                passed = checkChannels(mode, sound, dynamic) && passed;
        }
    }
    return passed ? 0 : 1;
}
