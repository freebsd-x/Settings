#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "shared/protocol.h"
#include <memory>
#include <cstring>
#include <iostream>

using namespace fbx::xkit;
using namespace fbx::shared;

static XBusClient* g_xbus     = nullptr;
static bool        g_dark_mode = false;
static int         g_accent_idx = 0;

// ─── helpers ────────────────────────────────────────────────────────────────

// Send the current dark_mode + accent_idx to the Theme service.
// All apps receive a ThemeChanged broadcast and re-render immediately.
static void theme_set(bool dark, int accent_idx) {
    if (!g_xbus) return;
    g_dark_mode   = dark;
    g_accent_idx  = accent_idx;
    ThemeSetRequest req{};
    req.dark_mode_   = dark ? 1u : 0u;
    req.accent_idx_  = static_cast<uint32_t>(accent_idx);
    g_xbus->Call("com.freebsdx.Theme",
        static_cast<uint32_t>(ThemeOpcode::SetTheme),
        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
        nullptr);
}

[[maybe_unused]] static void hw_set(HardwareOpcode op, uint32_t level) {
    if (!g_xbus) return;
    HardwareStatus hs{};
    if (op == HardwareOpcode::SetVolume)    hs.volume_level_    = level;
    if (op == HardwareOpcode::SetBacklight) hs.backlight_level_ = level;
    g_xbus->Call("com.freebsdx.Hardware", static_cast<uint32_t>(op),
        {reinterpret_cast<const uint8_t*>(&hs), sizeof(hs)}, [](const XBusMessage&) {});
}

static void cfg_set(const char* key, const char* value) {
    if (!g_xbus) return;
    ConfigValue cv{};
    std::strncpy(cv.key_,   key,   sizeof(cv.key_)   - 1);
    std::strncpy(cv.value_, value, sizeof(cv.value_) - 1);
    g_xbus->Call("com.freebsdx.Configuration", 0x02,
        {reinterpret_cast<const uint8_t*>(&cv), sizeof(cv)}, [](const XBusMessage&) {});
}

// Build a section card: the bold title row is a collapsible disclosure header
// (ExpanderWidget) that toggles the list of preference tiles below it. Expanded
// by default, so the page reads the same on open but any section can be folded
// away. Wrapped in CardWidget.
static std::shared_ptr<Widget> section(
    const std::string& title,
    std::vector<std::shared_ptr<Widget>> rows)
{
    auto header = std::make_shared<TextWidget>(title, 13.0f);
    header->setColor({0.55f, 0.55f, 0.6f, 1.0f});
    header->setPadding(4);

    std::vector<std::shared_ptr<Widget>> col;
    col.reserve(rows.size() * 2);
    for (size_t i = 0; i < rows.size(); ++i) {
        col.push_back(rows[i]);
        if (i + 1 < rows.size())
            col.push_back(std::make_shared<DividerWidget>(0.5f));
    }
    auto content = std::make_shared<FlexColumn>(std::move(col));

    auto expander = std::make_shared<ExpanderWidget>(header, content, /*expanded=*/true);
    return std::make_shared<CardWidget>(expander);
}

// ─── main ───────────────────────────────────────────────────────────────────

int main() {
    // ── Appearance ─────────────────────────────────────────────────────────
    auto dark_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        theme_set(v, g_accent_idx);
    });

    auto accent_seg = std::make_shared<SegmentedControlWidget>(
        std::vector<std::string>{"Blue", "Purple", "Orange"}, 0,
        [](int idx) { if (idx >= 0 && idx < 3) theme_set(g_dark_mode, idx); });

    auto tile_dark = std::make_shared<ListTileWidget>();
    tile_dark
        ->setTitle(std::make_shared<TextWidget>("Dark Mode", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("Use dark colour scheme", 12.0f))
        .setTrailing(dark_sw);

    auto tile_accent = std::make_shared<ListTileWidget>();
    tile_accent
        ->setTitle(std::make_shared<TextWidget>("Accent Colour", 15.0f))
        .setSubtitle(accent_seg);

    auto s_appearance = section("APPEARANCE", {tile_dark, tile_accent});

    // ── Wallpaper ──────────────────────────────────────────────────────────
    // Shows the current/selected path (filled by the file picker). Also a
    // fallback: typing a path + Enter applies it directly, no picker needed.
    auto wallpaper_path_input = std::make_shared<TextInputWidget>("");
    wallpaper_path_input->setPlaceholder("Type a path + Enter, or click Choose…");
    wallpaper_path_input->setFixedSize(300.0f, 36.0f);

    // Fit mode: 0=Cover, 1=Contain, 2=Stretch, 3=Center. Changing it re-applies
    // the current wallpaper live (no need to re-pick the image).
    auto wallpaper_fit = std::make_shared<DropdownWidget>(
        std::vector<std::string>{"Cover", "Contain", "Stretch", "Center"}, 0,
        [wallpaper_path_input](int idx) {
            if (!g_xbus) return;
            const std::string& path = wallpaper_path_input->text();
            if (path.empty()) return;  // nothing set yet
            WallpaperSetRequest req{};
            std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);
            req.fit_mode_ = static_cast<uint32_t>(idx >= 0 ? idx : 0);
            g_xbus->Call("com.freebsdx.Wallpaper",
                static_cast<uint32_t>(WallpaperOpcode::SetWallpaper),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [](const XBusMessage&) {});
        });

    // Fallback path: typing an absolute path + Enter applies it with the current
    // fit mode, so the wallpaper is settable even without the file picker.
    wallpaper_path_input->setOnSubmit([wallpaper_fit](const std::string& path) {
        if (!g_xbus || path.empty()) return;
        WallpaperSetRequest req{};
        std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);
        int idx = wallpaper_fit->selected();
        req.fit_mode_ = static_cast<uint32_t>(idx >= 0 ? idx : 0);
        g_xbus->Call("com.freebsdx.Wallpaper",
            static_cast<uint32_t>(WallpaperOpcode::SetWallpaper),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });

    // Correlates the async FileChooser Response with this request (see OnBroadcast).
    auto wp_chooser_req = std::make_shared<uint32_t>(0);

    // "Choose…" opens the native file picker (image filter); the selection is
    // applied as the wallpaper in the FileChooser Response handler (OnBroadcast),
    // using whatever fit mode is selected at that point.
    auto wallpaper_set_btn = std::make_shared<ButtonWidget>("Choose…",
        [wp_chooser_req]() {
            if (!g_xbus) return;
            FileChooserRequest req{};
            req.opcode_ = static_cast<uint32_t>(FileChooserOpcode::OpenFile);
            std::strncpy(req.title_, "Choose Wallpaper", sizeof(req.title_) - 1);
            std::strncpy(req.filter_mime_, "image/*", sizeof(req.filter_mime_) - 1);
            static uint32_t s_wp_req_id = 2000;
            req.request_id_ = ++s_wp_req_id;
            *wp_chooser_req = s_wp_req_id;
            g_xbus->Call("com.freebsdx.FileChooser",
                static_cast<uint32_t>(FileChooserOpcode::OpenFile),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                nullptr);
        });

    auto wallpaper_clear_btn = std::make_shared<ButtonWidget>("Clear",
        [wallpaper_path_input]() {
            if (!g_xbus) return;
            wallpaper_path_input->setText("");
            WallpaperSetRequest req{};  // path_[0] == '\0' → clear wallpaper
            g_xbus->Call("com.freebsdx.Wallpaper",
                static_cast<uint32_t>(WallpaperOpcode::SetWallpaper),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [](const XBusMessage&) {});
        });

    // Explicit sizes so the buttons are reliably visible and hit-testable.
    wallpaper_set_btn->setFixedSize(104.0f, 34.0f);
    wallpaper_clear_btn->setFixedSize(76.0f, 34.0f);

    auto wallpaper_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        wallpaper_set_btn, std::make_shared<Spacer>(8, 1), wallpaper_clear_btn});

    // Only the path preview goes in the ListTile subtitle; the fit dropdown and
    // the button row are their own full-width section rows. Cramming all three
    // into the subtitle clipped the button row and left "Choose…" unclickable.
    auto tile_wallpaper = std::make_shared<ListTileWidget>();
    tile_wallpaper
        ->setTitle(std::make_shared<TextWidget>("Wallpaper", 15.0f))
        .setSubtitle(wallpaper_path_input);

    auto s_wallpaper = section("WALLPAPER",
        {tile_wallpaper, wallpaper_fit, wallpaper_btn_row});

    // ── Display ────────────────────────────────────────────────────────────
    auto brightness_slider = std::make_shared<SliderWidget>(0.75f, 0.0f, 1.0f,
        [](float v) {
            if (!g_xbus) return;
            BacklightState bs{};
            bs.level_ = static_cast<uint32_t>(v * 100.0f);
            g_xbus->Call("com.freebsdx.Backlight",
                static_cast<uint32_t>(BacklightOpcode::SetBrightness),
                {reinterpret_cast<const uint8_t*>(&bs), sizeof(bs)},
                [](const XBusMessage&) {});
        });

    auto tile_brightness = std::make_shared<ListTileWidget>();
    tile_brightness
        ->setTitle(std::make_shared<TextWidget>("Brightness", 15.0f))
        .setSubtitle(brightness_slider);

    auto nightshift_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        cfg_set("display.nightshift", v ? "1" : "0");
    });
    auto tile_nightshift = std::make_shared<ListTileWidget>();
    tile_nightshift
        ->setTitle(std::make_shared<TextWidget>("Night Shift", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("Reduce blue light after sunset", 12.0f))
        .setTrailing(nightshift_sw);

    // HiDPI scale selector
    auto scale_set = [](float sc) {
        if (!g_xbus) return;
        DisplayScaleRequest req{};
        req.output_idx_ = 0;
        req.scale_       = sc;
        g_xbus->Call("com.freebsdx.Display",
            static_cast<uint32_t>(DisplayOpcode::SetOutputScale),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    };

    auto scale_seg = std::make_shared<SegmentedControlWidget>(
        std::vector<std::string>{"1×", "1.5×", "2×"}, 0,
        [scale_set](int idx) {
            float factors[] = {1.0f, 1.5f, 2.0f};
            if (idx >= 0 && idx < 3) scale_set(factors[idx]);
        });

    auto tile_scale = std::make_shared<ListTileWidget>();
    tile_scale
        ->setTitle(std::make_shared<TextWidget>("Display Scale (HiDPI)", 15.0f))
        .setSubtitle(scale_seg);

    auto vrr_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        if (!g_xbus) return;
        DisplayVrrRequest req{};
        req.output_idx_ = 0;
        req.enabled_    = v ? 1u : 0u;
        g_xbus->Call("com.freebsdx.Display",
            static_cast<uint32_t>(DisplayOpcode::SetVrrEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    auto tile_vrr = std::make_shared<ListTileWidget>();
    tile_vrr
        ->setTitle(std::make_shared<TextWidget>("Variable Refresh Rate", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>(
            "Reduce tearing (FreeSync / G-Sync / AdaptiveSync)", 12.0f))
        .setTrailing(vrr_sw);

    auto hdr_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        if (!g_xbus) return;
        DisplayHdrRequest req{};
        req.output_idx_    = 0;
        req.enabled_       = v ? 1u : 0u;
        req.max_luminance_ = 1000u; // HDR10 standard: 1000 nits
        g_xbus->Call("com.freebsdx.Display",
            static_cast<uint32_t>(DisplayOpcode::SetHdrEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    auto tile_hdr = std::make_shared<ListTileWidget>();
    tile_hdr
        ->setTitle(std::make_shared<TextWidget>("HDR Output", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>(
            "High Dynamic Range — HDR10, 10-bit wide-gamut (BT.2020)", 12.0f))
        .setTrailing(hdr_sw);

    auto s_display = section("DISPLAY", {tile_brightness, tile_nightshift, tile_scale, tile_vrr, tile_hdr});

    // ── Sound ──────────────────────────────────────────────────────────────
    auto volume_slider = std::make_shared<SliderWidget>(0.8f, 0.0f, 1.0f,
        [](float v) {
            if (!g_xbus) return;
            AudioVolumeRequest req{};
            req.volume_ = static_cast<uint32_t>(v * 100.0f);
            req.muted_  = 0xFFFFFFFFu; // leave mute unchanged
            g_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::SetVolume),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [](const XBusMessage&) {});
        });

    auto tile_volume = std::make_shared<ListTileWidget>();
    tile_volume
        ->setTitle(std::make_shared<TextWidget>("Output Volume", 15.0f))
        .setSubtitle(volume_slider);

    auto mute_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        if (!g_xbus) return;
        AudioVolumeRequest req{};
        req.volume_ = 0xFFFFFFFFu; // leave level unchanged
        req.muted_  = v ? 1u : 0u;
        g_xbus->Call("com.freebsdx.Audio",
            static_cast<uint32_t>(AudioOpcode::SetVolume),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    auto tile_mute = std::make_shared<ListTileWidget>();
    tile_mute
        ->setTitle(std::make_shared<TextWidget>("Mute", 15.0f))
        .setTrailing(mute_sw);

    // Output device selector — populated from ListSinks on connect.
    auto sink_label = std::make_shared<TextWidget>("—", 12.0f);
    auto sink_col   = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{sink_label});

    auto tile_output = std::make_shared<ListTileWidget>();
    tile_output
        ->setTitle(std::make_shared<TextWidget>("Output Device", 15.0f))
        .setSubtitle(sink_col);

    // Per-app stream volumes — populated from ListStreams on connect + VolumeChanged.
    auto stream_col = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});

    auto tile_streams = std::make_shared<ListTileWidget>();
    tile_streams
        ->setTitle(std::make_shared<TextWidget>("App Volumes", 15.0f))
        .setSubtitle(stream_col);

    auto s_sound = section("SOUND", {tile_volume, tile_mute, tile_output, tile_streams});

    // ── Network ────────────────────────────────────────────────────────────
    // Status label — updated from GetStatus on connect and NetworkChanged broadcasts.
    auto wifi_status_label = std::make_shared<TextWidget>("Not connected", 12.0f);
    auto wifi_sw = std::make_shared<SwitchWidget>(true, [](bool v) {
        if (!g_xbus) return;
        if (v) return; // enabling is implicit in Connect
        g_xbus->Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::Disconnect), {},
            [](const XBusMessage&) {});
    });
    auto tile_wifi = std::make_shared<ListTileWidget>();
    tile_wifi
        ->setTitle(std::make_shared<TextWidget>("Wi-Fi", 15.0f))
        .setSubtitle(wifi_status_label)
        .setTrailing(wifi_sw);

    // Network list — populated by Scan; rebuilt on NetworkChanged broadcast.
    auto network_list_col = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});

    // Passphrase entry for secured networks (hidden until user taps a secured SSID).
    auto passphrase_input = std::make_shared<TextInputWidget>("");
    passphrase_input->setPlaceholder("Passphrase");
    auto selected_ssid   = std::make_shared<std::string>();
    auto connect_btn     = std::make_shared<ButtonWidget>("Connect",
        [selected_ssid, passphrase_input]() {
            if (!g_xbus || selected_ssid->empty()) return;
            WifiConnectRequest req{};
            std::strncpy(req.ssid_, selected_ssid->c_str(), sizeof(req.ssid_) - 1);
            std::strncpy(req.passphrase_, passphrase_input->text().c_str(),
                         sizeof(req.passphrase_) - 1);
            g_xbus->Call("com.freebsdx.Network",
                static_cast<uint32_t>(NetworkOpcode::Connect),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [](const XBusMessage&) {});
        });
    auto passphrase_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{passphrase_input, connect_btn});

    auto scan_btn = std::make_shared<ButtonWidget>("Scan", [network_list_col,
            passphrase_row, passphrase_input, selected_ssid]() {
        if (!g_xbus) return;
        network_list_col->clearChildren();
        network_list_col->addChild(std::make_shared<TextWidget>("Scanning…", 12.0f));
        WifiScanRequest req{};
        req.page_ = 0;
        g_xbus->Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::Scan),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });

    auto tile_networks = std::make_shared<ListTileWidget>();
    tile_networks
        ->setTitle(std::make_shared<TextWidget>("Networks", 15.0f))
        .setSubtitle(std::make_shared<FlexColumn>(
            std::vector<std::shared_ptr<Widget>>{network_list_col, passphrase_row}))
        .setTrailing(scan_btn);

    // Helper: build/rebuild the SSID list from a received scan page.
    auto rebuild_network_list = [network_list_col, passphrase_row,
                                  passphrase_input, selected_ssid]
                                 (const WifiScanPage& pg) {
        if (pg.page_ == 0) network_list_col->clearChildren();
        if (pg.valid_) {
            const auto& net = pg.network_;
            // Signal bars: > -50 = 4, > -65 = 3, > -80 = 2, else 1
            int bars = (net.signal_strength_ > -50) ? 4 :
                       (net.signal_strength_ > -65) ? 3 :
                       (net.signal_strength_ > -80) ? 2 : 1;
            std::string bar_str(bars, '#');
            bar_str += std::string(4 - bars, '-');
            std::string lock = net.is_secure_ ? " 🔒" : "";
            std::string label = std::string(net.ssid_) + "  " + bar_str + lock;
            auto ssid_btn = std::make_shared<ButtonWidget>(label,
                [selected_ssid, passphrase_input, passphrase_row, net]() {
                    *selected_ssid = net.ssid_;
                    passphrase_input->setText("");
                    if (!net.is_secure_) {
                        // Open network: connect immediately
                        if (!g_xbus) return;
                        WifiConnectRequest req{};
                        std::strncpy(req.ssid_, net.ssid_, sizeof(req.ssid_) - 1);
                        g_xbus->Call("com.freebsdx.Network",
                            static_cast<uint32_t>(NetworkOpcode::Connect),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [](const XBusMessage&) {});
                    }
                    // Show passphrase row for secured networks
                });
            network_list_col->addChild(ssid_btn);
        }
    };

    auto s_network = section("NETWORK", {tile_wifi, tile_networks});

    // ── VPN ────────────────────────────────────────────────────────────────
    auto vpn_status_label = std::make_shared<TextWidget>("Not connected", 12.0f);

    auto vpn_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        if (!g_xbus) return;
        VpnConnectRequest req{};  // interface_="" → auto-select first .conf
        g_xbus->Call("com.freebsdx.VPN",
            static_cast<uint32_t>(v ? VpnOpcode::Connect : VpnOpcode::Disconnect),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });

    auto tile_vpn = std::make_shared<ListTileWidget>();
    tile_vpn
        ->setTitle(std::make_shared<TextWidget>("WireGuard VPN", 15.0f))
        .setSubtitle(vpn_status_label)
        .setTrailing(vpn_sw);

    // Import config: TextInput for path + Import button.
    // Also accepts drag-and-drop of a .conf file from the Files app.
    auto vpn_import_input = std::make_shared<TextInputWidget>("");
    vpn_import_input->setPlaceholder("Path to .conf  (or drag & drop)");

    auto vpn_import_btn = std::make_shared<ButtonWidget>("Import",
        [vpn_import_input]() {
            if (!g_xbus) return;
            VpnImportRequest req{};
            std::strncpy(req.path_, vpn_import_input->text().c_str(),
                         sizeof(req.path_) - 1);
            g_xbus->Call("com.freebsdx.VPN",
                static_cast<uint32_t>(VpnOpcode::ImportConfig),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [vpn_import_input](const XBusMessage& resp) {
                    VpnStatus st{};
                    std::memcpy(&st, resp.payload_, sizeof(st));
                    if (st.connected_ == 0xFFFFFFFFu)
                        vpn_import_input->setText("Import failed");
                    else
                        vpn_import_input->setText(
                            std::string("Imported: ") + st.interface_);
                });
        });

    auto vpn_import_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{vpn_import_input, vpn_import_btn});

    auto tile_vpn_import = std::make_shared<ListTileWidget>();
    tile_vpn_import
        ->setTitle(std::make_shared<TextWidget>("Import Config", 15.0f))
        .setSubtitle(vpn_import_row);

    auto s_vpn = section("VPN", {tile_vpn, tile_vpn_import});

    // ── Bluetooth ──────────────────────────────────────────────────────────
    auto bt_sw = std::make_shared<SwitchWidget>(false, [](bool v) {
        if (!g_xbus) return;
        BluetoothEnableRequest req{};
        req.enabled_ = v ? 1u : 0u;
        g_xbus->Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    auto tile_bt = std::make_shared<ListTileWidget>();
    tile_bt
        ->setTitle(std::make_shared<TextWidget>("Bluetooth", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("Enable adapter", 12.0f))
        .setTrailing(bt_sw);

    // Device list — populated by ListDevices + DeviceFound broadcasts
    auto bt_device_col = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});

    auto bt_scan_btn = std::make_shared<ButtonWidget>("Scan", [bt_device_col]() {
        if (!g_xbus) return;
        // Clear stale entries and trigger scan
        bt_device_col->clearChildren();
        bt_device_col->addChild(
            std::make_shared<TextWidget>("Scanning…", 12.0f));
        g_xbus->Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::Scan), {},
            [](const XBusMessage&) {});
    });

    auto tile_bt_scan = std::make_shared<ListTileWidget>();
    tile_bt_scan
        ->setTitle(std::make_shared<TextWidget>("Devices", 15.0f))
        .setSubtitle(bt_device_col)
        .setTrailing(bt_scan_btn);

    auto s_bluetooth = section("BLUETOOTH", {tile_bt, tile_bt_scan});

    // ── System ─────────────────────────────────────────────────────────────
    auto tile_update = std::make_shared<ListTileWidget>();
    tile_update
        ->setTitle(std::make_shared<TextWidget>("Software Update", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("FreeBSD-X 15.0", 12.0f))
        .setOnTap([]() {
            if (!g_xbus) return;
            g_xbus->Call("com.freebsdx.Update", 0x01, {}, [](const XBusMessage&) {
                std::cout << "[Settings] Update check triggered.\n";
            });
        });

    auto analytics_sw = std::make_shared<SwitchWidget>(true, [](bool v) {
        cfg_set("system.analytics", v ? "1" : "0");
    });
    auto tile_analytics = std::make_shared<ListTileWidget>();
    tile_analytics
        ->setTitle(std::make_shared<TextWidget>("Send Diagnostics", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("Help improve FreeBSD-X", 12.0f))
        .setTrailing(analytics_sw);

    auto tile_about = std::make_shared<ListTileWidget>();
    tile_about
        ->setTitle(std::make_shared<TextWidget>("About This Device", 15.0f))
        .setSubtitle(std::make_shared<TextWidget>("FreeBSD 15.0-RELEASE  •  xkit 1.0", 12.0f));

    auto s_system = section("SYSTEM", {tile_update, tile_analytics, tile_about});

    // ── Power ──────────────────────────────────────────────────────────────
    // Battery status — label updated from GetPowerState on connect.
    auto battery_label = std::make_shared<TextWidget>("Battery: —", 12.0f);
    auto tile_battery = std::make_shared<ListTileWidget>();
    tile_battery
        ->setTitle(std::make_shared<TextWidget>("Battery", 15.0f))
        .setSubtitle(battery_label);

    // Sleep timeout — Never / 5 m / 15 m / 30 m / 1 hr, as a compact stepper
    // (index 0..4; default 15 m). The formatter shows the label; onChanged maps
    // the index to the backend's seconds value.
    static const char* kSleepLabels[] = {"Never", "5 m", "15 m", "30 m", "1 hr"};
    auto sleep_stepper = std::make_shared<StepperWidget>(2, 0, 4, 1, [](float v) {
        if (!g_xbus) return;
        int idx = static_cast<int>(v);
        static const uint32_t kSeconds[] = {0, 5*60, 15*60, 30*60, 60*60};
        PowerSleepTimeoutRequest req{};
        req.seconds_ = kSeconds[idx >= 0 && idx < 5 ? idx : 2];
        g_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::SetSleepTimeout),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    sleep_stepper->setWidth(150.0f);
    sleep_stepper->setFormatter([](float v) {
        int idx = static_cast<int>(v);
        return std::string(kSleepLabels[idx >= 0 && idx < 5 ? idx : 0]);
    });
    auto tile_sleep = std::make_shared<ListTileWidget>();
    tile_sleep
        ->setTitle(std::make_shared<TextWidget>("Sleep After", 15.0f))
        .setSubtitle(sleep_stepper);

    // Lid close action
    auto lid_group = RadioGroupController::create();
    auto lid_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<RadioButtonWidget>(lid_group, 0),
        std::make_shared<TextWidget>("Nothing  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lid_group, 1),
        std::make_shared<TextWidget>("Sleep  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lid_group, 2),
        std::make_shared<TextWidget>("Hibernate  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lid_group, 3),
        std::make_shared<TextWidget>("Shut Down", 13.0f),
    });
    lid_group->select(1); // default: Sleep
    lid_group->subscribe([](int idx) {
        if (!g_xbus) return;
        PowerLidPolicyRequest req{};
        req.action_ = static_cast<uint32_t>(idx);
        g_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::SetLidPolicy),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });
    auto tile_lid = std::make_shared<ListTileWidget>();
    tile_lid
        ->setTitle(std::make_shared<TextWidget>("When Lid Closes", 15.0f))
        .setSubtitle(lid_row);

    // Power action buttons
    auto btn_sleep = std::make_shared<ButtonWidget>("Sleep", [](){
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::Suspend), {}, [](const XBusMessage&) {});
    });
    auto btn_hibernate = std::make_shared<ButtonWidget>("Hibernate", [](){
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::Hibernate), {}, [](const XBusMessage&) {});
    });
    auto btn_restart = std::make_shared<ButtonWidget>("Restart", [](){
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::Reboot), {}, [](const XBusMessage&) {});
    });
    auto btn_shutdown = std::make_shared<ButtonWidget>("Shut Down", [](){
        if (!g_xbus) return;
        g_xbus->Call("com.freebsdx.System",
            static_cast<uint32_t>(SystemOpcode::Shutdown), {}, [](const XBusMessage&) {});
    });
    auto power_buttons = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        btn_sleep, std::make_shared<Spacer>(8, 1),
        btn_hibernate, std::make_shared<Spacer>(8, 1),
        btn_restart, std::make_shared<Spacer>(8, 1),
        btn_shutdown,
    });
    auto tile_power_btns = std::make_shared<ListTileWidget>();
    tile_power_btns->setSubtitle(power_buttons);

    auto s_power = section("POWER", {tile_battery, tile_sleep, tile_lid, tile_power_btns});

    // ── Power Profiles ─────────────────────────────────────────────────────────
    auto cpu_freq_label = std::make_shared<TextWidget>("CPU: — MHz", 12.0f);
    cpu_freq_label->setColor(Color{0.55f, 0.55f, 0.6f, 1.0f});

    auto profile_group = RadioGroupController::create();
    auto profile_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<RadioButtonWidget>(profile_group, 0),
        std::make_shared<TextWidget>("Performance  ", 13.0f),
        std::make_shared<RadioButtonWidget>(profile_group, 1),
        std::make_shared<TextWidget>("Balanced  ", 13.0f),
        std::make_shared<RadioButtonWidget>(profile_group, 2),
        std::make_shared<TextWidget>("Power-Saver  ", 13.0f),
        std::make_shared<RadioButtonWidget>(profile_group, 3),
        std::make_shared<TextWidget>("Emergency", 13.0f),
    });
    profile_group->select(1); // default: Balanced
    profile_group->subscribe([](int idx) {
        if (!g_xbus || idx < 0 || idx > 3) return;
        PowerProfileRequest req{};
        req.profile_ = static_cast<uint32_t>(idx);
        g_xbus->Call("com.freebsdx.PowerProfile",
            static_cast<uint32_t>(PowerProfileOpcode::SetProfile),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [](const XBusMessage&) {});
    });

    auto tile_profile = std::make_shared<ListTileWidget>();
    tile_profile
        ->setTitle(std::make_shared<TextWidget>("CPU Power Profile", 15.0f))
        .setSubtitle(profile_row);

    auto tile_cpu_freq = std::make_shared<ListTileWidget>();
    tile_cpu_freq->setSubtitle(cpu_freq_label);

    auto s_power_profiles = section("POWER PROFILES", {tile_profile, tile_cpu_freq});

    // ── Seats ──────────────────────────────────────────────────────────────────
    auto seats_label = std::make_shared<TextWidget>("Loading seat information…", 12.0f);
    seats_label->setColor(Color{0.55f, 0.55f, 0.6f, 1.0f});

    auto tile_seats_info = std::make_shared<ListTileWidget>();
    tile_seats_info->setSubtitle(seats_label);

    auto s_seats = section("SEATS", {tile_seats_info});

    // ── Lock Screen ────────────────────────────────────────────────────────────
    auto lock_group = RadioGroupController::create();
    static const uint32_t kLockSeconds[] = {0, 1*60, 5*60, 10*60, 30*60, 60*60};
    auto lock_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<RadioButtonWidget>(lock_group, 0),
        std::make_shared<TextWidget>("Never  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lock_group, 1),
        std::make_shared<TextWidget>("1 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lock_group, 2),
        std::make_shared<TextWidget>("5 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lock_group, 3),
        std::make_shared<TextWidget>("10 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lock_group, 4),
        std::make_shared<TextWidget>("30 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(lock_group, 5),
        std::make_shared<TextWidget>("1 hr", 13.0f),
    });
    lock_group->select(3); // default: 10 min
    lock_group->subscribe([](int idx) {
        if (!g_xbus) return;
        LockTimeoutRequest req{};
        req.seconds_ = kLockSeconds[idx < 6 ? idx : 3];
        g_xbus->Call("com.freebsdx.Lock",
            static_cast<uint32_t>(LockOpcode::SetTimeout),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            nullptr);
    });
    auto tile_lock_timeout = std::make_shared<ListTileWidget>();
    tile_lock_timeout
        ->setTitle(std::make_shared<TextWidget>("Lock Screen After", 15.0f))
        .setSubtitle(lock_row);

    auto btn_lock_now = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Lock Now", 14.0f),
        []() {
            if (!g_xbus) return;
            g_xbus->Call("com.freebsdx.Lock",
                static_cast<uint32_t>(LockOpcode::Lock), {}, nullptr);
        });
    auto tile_lock_now = std::make_shared<ListTileWidget>();
    tile_lock_now->setSubtitle(btn_lock_now);

    auto s_lock = section("LOCK SCREEN", {tile_lock_timeout, tile_lock_now});

    // ── Screen Saver ───────────────────────────────────────────────────────────
    // Shared config struct lets the timeout and mode radio groups stay in sync
    // without requiring read-modify-write round-trips via XBus.
    auto ss_config = std::make_shared<ScreenSaverConfig>();
    ss_config->timeout_secs_ = 5 * 60; // mirrors ScreenSaverService default
    ss_config->mode_ = 0;

    auto ss_sw = std::make_shared<SwitchWidget>(true); // enabled by default
    ss_sw->setOnChanged([ss_config](bool on) {
        if (!g_xbus) return;
        ss_config->timeout_secs_ = on ? (5 * 60) : 0;
        g_xbus->Call("com.freebsdx.ScreenSaver",
            static_cast<uint32_t>(ScreenSaverOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(ss_config.get()), sizeof(*ss_config)},
            nullptr);
    });
    auto tile_ss_enabled = std::make_shared<ListTileWidget>();
    tile_ss_enabled
        ->setTitle(std::make_shared<TextWidget>("Enable Screen Saver", 15.0f))
        .setTrailing(ss_sw);

    auto ss_timeout_group = RadioGroupController::create();
    static const uint32_t kSaverSeconds[] = {60, 2*60, 5*60, 10*60, 30*60};
    auto ss_timeout_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<RadioButtonWidget>(ss_timeout_group, 0),
        std::make_shared<TextWidget>("1 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_timeout_group, 1),
        std::make_shared<TextWidget>("2 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_timeout_group, 2),
        std::make_shared<TextWidget>("5 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_timeout_group, 3),
        std::make_shared<TextWidget>("10 m  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_timeout_group, 4),
        std::make_shared<TextWidget>("30 m", 13.0f),
    });
    ss_timeout_group->select(2); // default: 5 min
    ss_timeout_group->subscribe([ss_config](int idx) {
        if (!g_xbus) return;
        ss_config->timeout_secs_ = kSaverSeconds[idx < 5 ? idx : 2];
        g_xbus->Call("com.freebsdx.ScreenSaver",
            static_cast<uint32_t>(ScreenSaverOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(ss_config.get()), sizeof(*ss_config)},
            nullptr);
    });
    auto tile_ss_timeout = std::make_shared<ListTileWidget>();
    tile_ss_timeout
        ->setTitle(std::make_shared<TextWidget>("Start After", 15.0f))
        .setSubtitle(ss_timeout_row);

    auto ss_mode_group = RadioGroupController::create();
    auto ss_mode_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<RadioButtonWidget>(ss_mode_group, 0),
        std::make_shared<TextWidget>("Blank  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_mode_group, 1),
        std::make_shared<TextWidget>("Clock  ", 13.0f),
        std::make_shared<RadioButtonWidget>(ss_mode_group, 2),
        std::make_shared<TextWidget>("Matrix Rain", 13.0f),
    });
    ss_mode_group->select(0); // default: blank
    ss_mode_group->subscribe([ss_config](int idx) {
        if (!g_xbus) return;
        ss_config->mode_ = static_cast<uint32_t>(idx < 3 ? idx : 0);
        g_xbus->Call("com.freebsdx.ScreenSaver",
            static_cast<uint32_t>(ScreenSaverOpcode::SetConfig),
            {reinterpret_cast<const uint8_t*>(ss_config.get()), sizeof(*ss_config)},
            nullptr);
    });
    auto tile_ss_mode = std::make_shared<ListTileWidget>();
    tile_ss_mode
        ->setTitle(std::make_shared<TextWidget>("Style", 15.0f))
        .setSubtitle(ss_mode_row);

    auto s_screensaver = section("SCREEN SAVER",
                                 {tile_ss_enabled, tile_ss_timeout, tile_ss_mode});

    // ── Remote Desktop ─────────────────────────────────────────────────────
    auto rdp_sw = std::make_shared<SwitchWidget>(false);
    rdp_sw->setOnChanged([](bool on) {
        if (!g_xbus) return;
        if (on)
            g_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopOpcode::Enable), {}, nullptr);
        else
            g_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopOpcode::Disable), {}, nullptr);
    });
    auto tile_rdp = std::make_shared<ListTileWidget>();
    tile_rdp->setTitle(std::make_shared<TextWidget>("Enable Remote Desktop (VNC)", 15.0f))
            .setSubtitle(std::make_shared<TextWidget>("Allow VNC clients on port 5900", 12.0f))
            .setTrailing(rdp_sw);

    auto rdp_pw_input = std::make_shared<TextInputWidget>("");
    rdp_pw_input->setPlaceholder("VNC password (leave blank for none)");
    rdp_pw_input->setFixedSize(220.0f, 36.0f);
    auto rdp_pw_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Set", 13.0f),
        [rdp_pw_input]() {
            if (!g_xbus) return;
            RemoteDesktopPasswordRequest req{};
            std::strncpy(req.password_, rdp_pw_input->text().c_str(),
                         sizeof(req.password_) - 1);
            g_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopOpcode::SetPassword),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                nullptr);
        });
    auto rdp_pw_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        rdp_pw_input, std::make_shared<Spacer>(8, 1), rdp_pw_btn});
    auto tile_rdp_pw = std::make_shared<ListTileWidget>();
    tile_rdp_pw->setTitle(std::make_shared<TextWidget>("VNC Password", 15.0f))
               .setSubtitle(rdp_pw_row);

    auto rdp_status_w = std::make_shared<TextWidget>("VNC server off", 13.0f);
    rdp_status_w->setColor(Color{0.6f, 0.6f, 0.6f, 1.0f});
    auto tile_rdp_status = std::make_shared<ListTileWidget>();
    tile_rdp_status->setSubtitle(rdp_status_w);

    auto s_rdp = section("REMOTE DESKTOP", {tile_rdp, tile_rdp_pw, tile_rdp_status});

    // ── Root layout ────────────────────────────────────────────────────────
    auto title_bar = std::make_shared<TextWidget>("Settings", 28.0f);
    title_bar->setPadding(8);

    auto content = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        title_bar,
        std::make_shared<Spacer>(1, 12),
        s_appearance,
        std::make_shared<Spacer>(1, 8),
        s_wallpaper,
        std::make_shared<Spacer>(1, 8),
        s_display,
        std::make_shared<Spacer>(1, 8),
        s_sound,
        std::make_shared<Spacer>(1, 8),
        s_network,
        std::make_shared<Spacer>(1, 8),
        s_vpn,
        std::make_shared<Spacer>(1, 8),
        s_bluetooth,
        std::make_shared<Spacer>(1, 8),
        s_power,
        std::make_shared<Spacer>(1, 8),
        s_power_profiles,
        std::make_shared<Spacer>(1, 8),
        s_seats,
        std::make_shared<Spacer>(1, 8),
        s_lock,
        std::make_shared<Spacer>(1, 8),
        s_screensaver,
        std::make_shared<Spacer>(1, 8),
        s_rdp,
        std::make_shared<Spacer>(1, 8),
        s_system,
        std::make_shared<Spacer>(1, 24),
    });
    content->setPadding(20);

    auto root = std::make_shared<ScrollViewWidget>(content);

    return XApplication::Run(root, "/tmp/freebsdx.sock", true, false,
        [dark_sw, accent_seg, battery_label, sleep_stepper, lid_group,
         brightness_slider, bt_sw, bt_device_col,
         wifi_status_label, wifi_sw, rebuild_network_list,
         vpn_status_label, vpn_sw, vpn_import_input,
         volume_slider, mute_sw, sink_col, sink_label, stream_col,
         lock_group, rdp_sw, rdp_status_w,
         ss_config, ss_sw, ss_timeout_group, ss_mode_group,
         profile_group, cpu_freq_label, seats_label,
         wallpaper_path_input, wallpaper_fit, wp_chooser_req](XBusClient& xbus) {
            g_xbus = &xbus;
            XApplication::setTitle("Settings");

            // ── Wallpaper: restore path + fit mode from server ────────────────
            g_xbus->Call("com.freebsdx.Wallpaper",
                static_cast<uint32_t>(WallpaperOpcode::GetWallpaper), {},
                [wallpaper_path_input, wallpaper_fit](const XBusMessage& resp) {
                    WallpaperSetRequest r{};
                    std::memcpy(&r, resp.payload_, sizeof(r));
                    if (r.path_[0])
                        wallpaper_path_input->setText(std::string(r.path_));
                    if (r.fit_mode_ < 4)
                        wallpaper_fit->setSelected(static_cast<int>(r.fit_mode_));
                });

            // ── Audio initialisation ──────────────────────────────────────────
            // Helper: rebuild per-app stream sliders from ListStreams pages.
            auto rebuild_streams = [stream_col](const XBusMessage& resp) {
                AudioStreamPage pg{};
                std::memcpy(&pg, resp.payload_, sizeof(pg));
                if (pg.page_ == 0) stream_col->clearChildren();
                if (pg.valid_) {
                    const auto& s = pg.stream_;
                    auto lbl = std::make_shared<TextWidget>(
                        std::string(s.app_name_) + ": " +
                        std::to_string(s.volume_) + "%", 12.0f);
                    uint32_t sid = s.stream_id_;
                    uint32_t vol = s.volume_;
                    auto slider = std::make_shared<SliderWidget>(
                        static_cast<float>(vol) / 100.0f, 0.0f, 1.0f,
                        [sid](float v) {
                            if (!g_xbus) return;
                            AudioAppVolumeRequest req{};
                            req.stream_id_ = sid;
                            req.volume_    = static_cast<uint32_t>(v * 100.0f);
                            g_xbus->Call("com.freebsdx.Audio",
                                static_cast<uint32_t>(AudioOpcode::SetAppVolume),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                [](const XBusMessage&) {});
                        });
                    auto row = std::make_shared<FlexColumn>(
                        std::vector<std::shared_ptr<Widget>>{lbl, slider});
                    stream_col->addChild(row);
                    // Request next page if more streams exist
                    if (pg.has_more_ && g_xbus) {
                        uint32_t next = pg.page_ + 1;
                        g_xbus->Call("com.freebsdx.Audio",
                            static_cast<uint32_t>(AudioOpcode::ListStreams),
                            {reinterpret_cast<const uint8_t*>(&next), sizeof(next)},
                            [stream_col](const XBusMessage& r2) {
                                // Handled inline — single level of recursion is safe
                                // (streams per Settings session are typically ≤ 4)
                                AudioStreamPage pg2{};
                                std::memcpy(&pg2, r2.payload_, sizeof(pg2));
                                if (pg2.valid_) {
                                    stream_col->addChild(std::make_shared<TextWidget>(
                                        std::string(pg2.stream_.app_name_) + ": " +
                                        std::to_string(pg2.stream_.volume_) + "%", 12.0f));
                                }
                            });
                    }
                }
                if (!pg.valid_ && pg.total_ == 0)
                    stream_col->addChild(
                        std::make_shared<TextWidget>("No active streams", 12.0f));
            };

            // Fetch master volume and initialise slider + mute toggle.
            g_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::GetVolume),
                {},
                [volume_slider, mute_sw](const XBusMessage& resp) {
                    AudioVolumeState vs{};
                    std::memcpy(&vs, resp.payload_, sizeof(vs));
                    volume_slider->setValue(static_cast<float>(vs.volume_) / 100.0f);
                    mute_sw->setValue(vs.muted_ != 0);
                });

            // Populate output device list (page 0).
            g_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::ListSinks),
                {},
                [sink_col, sink_label](const XBusMessage& resp) {
                    AudioDevicePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    sink_col->clearChildren();
                    if (pg.valid_) {
                        auto btn = std::make_shared<ButtonWidget>(
                            std::string(pg.device_.name_) +
                            (pg.device_.is_default_ ? "  ✓" : ""),
                            [dev = pg.device_]() {
                                if (!g_xbus) return;
                                AudioDeviceRequest req{};
                                std::strncpy(req.path_, dev.path_, sizeof(req.path_) - 1);
                                g_xbus->Call("com.freebsdx.Audio",
                                    static_cast<uint32_t>(AudioOpcode::SetDefaultSink),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    [](const XBusMessage&) {});
                            });
                        sink_col->addChild(btn);
                    }
                    if (!pg.valid_ || pg.total_ == 0)
                        sink_col->addChild(
                            std::make_shared<TextWidget>("No output devices", 12.0f));
                });

            // Load page 0 of active streams.
            g_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::ListStreams),
                {},
                rebuild_streams);

            // Subscribe to broadcasts: VolumeChanged, VpnChanged, NetworkChanged,
            // ProfileChanged, RemoteDesktop StatusChanged — all handled in one callback.
            g_xbus->OnBroadcast([volume_slider, mute_sw, rebuild_streams,
                                  vpn_status_label, vpn_sw,
                                  wifi_status_label, wifi_sw, rebuild_network_list,
                                  profile_group, cpu_freq_label,
                                  rdp_sw, rdp_status_w,
                                  wallpaper_path_input, wallpaper_fit,
                                  wp_chooser_req](const XBusMessage& bcast) {
                std::string svc(bcast.service_name_);
                uint32_t op = bcast.opcode_;

                // ── Wallpaper file picker result ──────────────────────────────
                // Apply the chosen image with the currently-selected fit mode.
                if (svc == "com.freebsdx.FileChooser" &&
                        op == static_cast<uint32_t>(FileChooserOpcode::Response)) {
                    FileChooserResponse resp{};
                    std::memcpy(&resp, bcast.payload_, sizeof(resp));
                    if (resp.request_id_ != *wp_chooser_req || resp.cancelled_) return;
                    if (resp.path_[0] == '\0') return;
                    wallpaper_path_input->setText(std::string(resp.path_));
                    WallpaperSetRequest wreq{};
                    std::strncpy(wreq.path_, resp.path_, sizeof(wreq.path_) - 1);
                    int idx = wallpaper_fit->selected();
                    wreq.fit_mode_ = static_cast<uint32_t>(idx >= 0 ? idx : 0);
                    g_xbus->Call("com.freebsdx.Wallpaper",
                        static_cast<uint32_t>(WallpaperOpcode::SetWallpaper),
                        {reinterpret_cast<const uint8_t*>(&wreq), sizeof(wreq)},
                        [](const XBusMessage&) {});
                    return;
                }

                if (svc == "com.freebsdx.Audio" &&
                        op == static_cast<uint32_t>(AudioOpcode::VolumeChanged)) {
                    AudioVolumeState vs{};
                    std::memcpy(&vs, bcast.payload_, sizeof(vs));
                    volume_slider->setValue(static_cast<float>(vs.volume_) / 100.0f);
                    mute_sw->setValue(vs.muted_ != 0);
                    if (g_xbus) {
                        uint32_t pg0 = 0;
                        g_xbus->Call("com.freebsdx.Audio",
                            static_cast<uint32_t>(AudioOpcode::ListStreams),
                            {reinterpret_cast<const uint8_t*>(&pg0), sizeof(pg0)},
                            rebuild_streams);
                    }
                } else if (svc == "com.freebsdx.VPN" &&
                        op == static_cast<uint32_t>(VpnOpcode::VpnChanged)) {
                    VpnStatus st{};
                    std::memcpy(&st, bcast.payload_, sizeof(st));
                    vpn_sw->setValue(st.connected_ != 0);
                    if (st.connected_) {
                        std::string lbl = std::string(st.interface_);
                        if (st.ip_address_[0])
                            lbl += "  " + std::string(st.ip_address_);
                        vpn_status_label->setText(lbl);
                    } else {
                        vpn_status_label->setText("Not connected");
                    }
                } else if (svc == "com.freebsdx.Network" &&
                        op == static_cast<uint32_t>(NetworkOpcode::NetworkChanged)) {
                    NetworkStatus st{};
                    std::memcpy(&st, bcast.payload_, sizeof(st));
                    wifi_sw->setValue(st.is_connected_ != 0);
                    if (st.is_connected_) {
                        std::string lbl = std::string(st.current_ssid_)
                                        + "  " + std::string(st.ip_address_);
                        wifi_status_label->setText(lbl);
                    } else {
                        wifi_status_label->setText("Not connected");
                    }
                    if (!g_xbus) return;
                    WifiScanRequest req{};
                    g_xbus->Call("com.freebsdx.Network",
                        static_cast<uint32_t>(NetworkOpcode::Scan),
                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                        [rebuild_network_list](const XBusMessage& resp) {
                            WifiScanPage pg{};
                            std::memcpy(&pg, resp.payload_, sizeof(pg));
                            rebuild_network_list(pg);
                        });
                } else if (svc == "com.freebsdx.PowerProfile" &&
                        op == static_cast<uint32_t>(PowerProfileOpcode::ProfileChanged)) {
                    PowerProfileState ps{};
                    std::memcpy(&ps, bcast.payload_, sizeof(ps));
                    profile_group->select(static_cast<int>(ps.profile_));
                    if (ps.cpu_freq_mhz_ > 0) {
                        std::string txt = "CPU: " + std::to_string(ps.cpu_freq_mhz_) + " MHz";
                        if (ps.cpu_max_mhz_ > 0)
                            txt += "  (max " + std::to_string(ps.cpu_max_mhz_) + " MHz)";
                        cpu_freq_label->setText(txt);
                    }
                } else if (svc == "com.freebsdx.RemoteDesktop" &&
                        op == static_cast<uint32_t>(RemoteDesktopOpcode::StatusChanged)) {
                    RemoteDesktopStatus st{};
                    std::memcpy(&st, bcast.payload_, sizeof(st));
                    rdp_sw->setValue(st.running_ != 0);
                    if (st.running_) {
                        std::string lbl = "VNC on port " +
                            std::to_string(st.port_) + " — " +
                            std::to_string(st.client_count_) + " client(s)";
                        if (st.password_set_) lbl += " [password]";
                        rdp_status_w->setText(lbl);
                    } else {
                        rdp_status_w->setText("VNC server off");
                    }
                }
            });

            // Fetch VPN status and subscribe to VpnChanged broadcasts.
            g_xbus->Call("com.freebsdx.VPN",
                static_cast<uint32_t>(VpnOpcode::GetStatus),
                {},
                [vpn_status_label, vpn_sw](const XBusMessage& resp) {
                    VpnStatus st{};
                    std::memcpy(&st, resp.payload_, sizeof(st));
                    vpn_sw->setValue(st.connected_ != 0);
                    if (st.connected_) {
                        std::string lbl = std::string(st.interface_);
                        if (st.ip_address_[0])
                            lbl += "  " + std::string(st.ip_address_);
                        vpn_status_label->setText(lbl);
                    } else {
                        vpn_status_label->setText("Not connected");
                    }
                });

            // Drag-and-drop: dropping a .conf from the Files app auto-fills the import path.
            XApplication::setOnDrop("text/uri-list",
                [vpn_import_input](float, float, const std::string& data) {
                    std::string path = data;
                    while (!path.empty() &&
                           (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
                        path.pop_back();
                    if (path.rfind("file://", 0) == 0) path = path.substr(7);
                    if (path.size() >= 5 && path.substr(path.size() - 5) == ".conf")
                        vpn_import_input->setText(path);
                });

            // Fetch Wi-Fi status and register for NetworkChanged broadcasts.
            g_xbus->Call("com.freebsdx.Network",
                static_cast<uint32_t>(NetworkOpcode::GetStatus),
                {},
                [wifi_status_label, wifi_sw](const XBusMessage& resp) {
                    NetworkStatus st{};
                    std::memcpy(&st, resp.payload_, sizeof(st));
                    wifi_sw->setValue(st.is_connected_ != 0);
                    if (st.is_connected_) {
                        std::string lbl = std::string(st.current_ssid_)
                                        + "  " + std::string(st.ip_address_);
                        wifi_status_label->setText(lbl);
                    } else {
                        wifi_status_label->setText("Not connected");
                    }
                });

            // Fetch Bluetooth adapter state and paired device list.
            g_xbus->Call("com.freebsdx.Bluetooth",
                static_cast<uint32_t>(BluetoothOpcode::GetState),
                {},
                [bt_sw](const XBusMessage& resp) {
                    BluetoothState st{};
                    std::memcpy(&st, resp.payload_, sizeof(st));
                    bt_sw->setValue(st.enabled_ != 0);
                });

            // Load page 0 of known devices (and recurse for further pages)
            std::function<void(uint32_t)> load_bt_page =
                [bt_device_col, &load_bt_page](uint32_t page) {
                if (!g_xbus) return;
                uint32_t pg = page;
                g_xbus->Call("com.freebsdx.Bluetooth",
                    static_cast<uint32_t>(BluetoothOpcode::ListDevices),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [bt_device_col, &load_bt_page, page](const XBusMessage& resp) {
                        BluetoothDevicePage pg_resp{};
                        std::memcpy(&pg_resp, resp.payload_, sizeof(pg_resp));
                        for (uint32_t i = 0; i < pg_resp.count_; ++i) {
                            const auto& dev = pg_resp.devices_[i];
                            std::string lbl = std::string(dev.name_) +
                                (dev.connected_ ? "  ●" : "  ○");
                            bt_device_col->addChild(
                                std::make_shared<TextWidget>(lbl, 12.0f));
                        }
                        if (pg_resp.has_more_)
                            load_bt_page(page + 1);
                    });
            };
            load_bt_page(0);

            // Initialise brightness slider from current hardware level.
            g_xbus->Call("com.freebsdx.Backlight",
                static_cast<uint32_t>(BacklightOpcode::GetBrightness),
                {},
                [brightness_slider](const XBusMessage& resp) {
                    BacklightState bs{};
                    std::memcpy(&bs, resp.payload_, sizeof(bs));
                    brightness_slider->setValue(
                        static_cast<float>(bs.level_) / 100.0f);
                });

            // Fetch persisted theme state and initialise controls to match.
            g_xbus->Call("com.freebsdx.Theme",
                static_cast<uint32_t>(ThemeOpcode::GetTheme),
                {},
                [dark_sw, accent_seg](const XBusMessage& resp) {
                    ThemeState ts{};
                    std::memcpy(&ts, resp.payload_, sizeof(ts));
                    g_dark_mode  = (ts.dark_mode_ != 0);
                    g_accent_idx = (ts.accent_idx_ <= 2u)
                                   ? static_cast<int>(ts.accent_idx_) : 0;
                    dark_sw->setValue(g_dark_mode);
                    accent_seg->setSelected(g_accent_idx);
                });

            // Fetch power state to populate battery tile and restore settings.
            g_xbus->Call("com.freebsdx.Power",
                static_cast<uint32_t>(PowerOpcode::GetPowerState),
                {},
                [battery_label](const XBusMessage& resp) {
                    PowerState ps{};
                    std::memcpy(&ps, resp.payload_, sizeof(ps));
                    std::string txt;
                    if (ps.battery_state_ == 3) {
                        txt = "No battery";
                    } else {
                        txt = std::to_string(ps.battery_life_) + "%";
                        if (ps.battery_state_ == 1)      txt += " — Charging";
                        else if (ps.battery_state_ == 2) txt += " — Full";
                        else if (ps.time_remaining_ != 0xFFFFFFFFu)
                            txt += " — " + std::to_string(ps.time_remaining_) + " min remaining";
                        txt += ps.on_ac_ ? "  (AC)" : "  (Battery)";
                    }
                    battery_label->setText(txt);
                });

            g_xbus->Call("com.freebsdx.Power",
                static_cast<uint32_t>(PowerOpcode::GetSleepTimeout),
                {},
                [sleep_stepper](const XBusMessage& resp) {
                    PowerSleepTimeoutRequest r{};
                    std::memcpy(&r, resp.payload_, sizeof(r));
                    static const uint32_t kSeconds[] = {0, 5*60, 15*60, 30*60, 60*60};
                    for (int i = 0; i < 5; ++i)
                        if (r.seconds_ == kSeconds[i]) { sleep_stepper->setValue(i); break; }
                });

            g_xbus->Call("com.freebsdx.Power",
                static_cast<uint32_t>(PowerOpcode::GetLidPolicy),
                {},
                [lid_group](const XBusMessage& resp) {
                    PowerLidPolicyRequest r{};
                    std::memcpy(&r, resp.payload_, sizeof(r));
                    lid_group->select(static_cast<int>(r.action_));
                });

            // Fetch current power profile and CPU freq
            g_xbus->Call("com.freebsdx.PowerProfile",
                static_cast<uint32_t>(PowerProfileOpcode::GetProfile),
                {},
                [profile_group, cpu_freq_label](const XBusMessage& resp) {
                    PowerProfileState ps{};
                    std::memcpy(&ps, resp.payload_, sizeof(ps));
                    profile_group->select(static_cast<int>(ps.profile_));
                    if (ps.cpu_freq_mhz_ > 0) {
                        std::string txt = "CPU: " + std::to_string(ps.cpu_freq_mhz_) + " MHz";
                        if (ps.cpu_max_mhz_ > 0)
                            txt += "  (max " + std::to_string(ps.cpu_max_mhz_) + " MHz)";
                        cpu_freq_label->setText(txt);
                    }
                });

            // Fetch seat list (page 0)
            g_xbus->Call("com.freebsdx.Seat",
                static_cast<uint32_t>(SeatOpcode::ListSeats),
                {},
                [seats_label](const XBusMessage& resp) {
                    SeatPage sp{};
                    std::memcpy(&sp, resp.payload_, sizeof(sp));
                    if (sp.valid_) {
                        std::string txt = std::string(sp.name_) + ":";
                        if (sp.has_keyboard_) txt += " keyboard";
                        if (sp.has_pointer_)  txt += " pointer";
                        if (sp.has_touch_)    txt += " touch";
                        if (sp.user_[0])      txt += "  (user: " + std::string(sp.user_) + ")";
                        if (sp.active_surface_)
                            txt += "  focus=" + std::to_string(sp.active_surface_);
                        seats_label->setText(txt);
                    } else {
                        seats_label->setText("No seats detected");
                    }
                });

            // Fetch initial Screen Saver config and sync controls.
            g_xbus->Call("com.freebsdx.ScreenSaver",
                static_cast<uint32_t>(ScreenSaverOpcode::GetConfig),
                {},
                [ss_config, ss_sw, ss_timeout_group, ss_mode_group](const XBusMessage& resp) {
                    ScreenSaverConfig cfg{};
                    std::memcpy(&cfg, resp.payload_, sizeof(cfg));
                    *ss_config = cfg;
                    ss_sw->setValue(cfg.timeout_secs_ > 0);
                    static const uint32_t kSecs[] = {60, 2*60, 5*60, 10*60, 30*60};
                    for (int i = 0; i < 5; ++i)
                        if (cfg.timeout_secs_ == kSecs[i]) { ss_timeout_group->select(i); break; }
                    if (cfg.mode_ < 3) ss_mode_group->select(cfg.mode_);
                });

            // Fetch initial Remote Desktop status
            g_xbus->Call("com.freebsdx.RemoteDesktop",
                static_cast<uint32_t>(RemoteDesktopOpcode::GetStatus),
                {},
                [rdp_sw, rdp_status_w](const XBusMessage& resp) {
                    RemoteDesktopStatus st{};
                    std::memcpy(&st, resp.payload_, sizeof(st));
                    rdp_sw->setValue(st.running_ != 0);
                    if (st.running_) {
                        std::string lbl = "VNC on port " +
                            std::to_string(st.port_) + " — " +
                            std::to_string(st.client_count_) + " client(s)";
                        if (st.password_set_) lbl += " [password]";
                        rdp_status_w->setText(lbl);
                    } else {
                        rdp_status_w->setText("VNC server off");
                    }
                });

        });
}
