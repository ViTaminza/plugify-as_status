// as_status — Plugify C++ port of the Pisex Metamod:Source module "[AS] Status" (as_status.cpp).
//
// Behaviour kept from the original:
//   * console command "mm_status": the server console (-1) always gets the player list on the
//     server console; a player calling it needs "@admin/status" and gets the list on their own
//     console;
//   * chat command "!status": players with "@admin/status" get the list in chat;
//   * the list is the original one: for every slot 0..63 that is not a fake client,
//     "ID. Name - SteamID64" (the original did not check connection state - empty slots show up
//     with an empty name and steamid 0, which is kept);
//   * no phrases: the original printed literal English text.
//
// Porting notes:
//   * The original's META_CONPRINTF (server console) and PrintToConsole(slot) are the same call
//     through pisexcompat: a negative slot prints to the server console.
//   * `%lld` with a uint64_t steamid is a format mismatch on Linux (uint64_t is unsigned long);
//     the value is printed through "%lu" with the identical decimal output.
//
// Nothing touches a client in OnPluginStart (commands are only dispatched with a live map);
// OnPluginStart never returns Failed - that would stop every Plugify plugin on the server.

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plugin_export.h>

#include <pisex/pisex.hpp>

#include <admin_api.hpp>

namespace {

constexpr int kMaxSlots = 64;

admin_system::AdminApi* g_pAdminApi = nullptr;

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// The original's Get_Status(): the same lines, only the output destination depends on bConsole
// (print to the slot's console vs. the slot's chat).
void GetStatus(int iSlot, bool bConsole) {
	if (bConsole) {
		pisex::g_pUtils->PrintToConsole(iSlot, "ID. Name - SteamID\n");
	} else {
		pisex::g_pUtils->PrintToChat(iSlot, "ID. Name - SteamID\n");
	}
	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) continue;
		const char* szName = pisex::g_pPlayers->GetPlayerName(i);
		const auto iSteam = static_cast<unsigned long>(pisex::g_pPlayers->GetSteamID64(i));
		if (bConsole) {
			pisex::g_pUtils->PrintToConsole(iSlot, "%i. %s - %lu\n", i, szName, iSteam);
		} else {
			pisex::g_pUtils->PrintToChat(iSlot, "%i. %s - %lu\n", i, szName, iSteam);
		}
	}
}

class AsStatusPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// The admin system family keeps its files in <plugify configs>/admin_system (fixed
			// layout); this also wires the s2sdk listeners of pisexcompat.
			pisex::Init("admin_system");
			g_pAdminApi = &admin_system::AdminApi::Get();

			// mm_status: the original's CON_COMMAND_F(..., FCVAR_GAMEDLL) — server console only.
			pisex::g_pUtils->RegServerCommand({"mm_status"}, [](int iSlot, const char*) -> bool {
				const bool bConsole = iSlot == -1;
				if (!g_pAdminApi->HasPermission(iSlot, "@admin/status") && !bConsole) {
					return true;
				}
				GetStatus(iSlot, true); // both the server console and a player's console
				return true;
			});

			// !status: chat command.
			pisex::g_pUtils->RegCommand({}, {"!status"}, [](int iSlot, const char*) -> bool {
				if (g_pAdminApi->HasPermission(iSlot, "@admin/status")) {
					GetStatus(iSlot, false);
				}
				return true;
			});

			LogMessage("as_status: ready ('mm_status' and '!status' registered)", plg::Severity::Info);
		} catch (const std::exception& ex) {
			LogError(std::string("as_status: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("as_status: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			pisex::Poll();
		} catch (...) {
			LogError("as_status: Poll() threw");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// Removes the two commands and the s2sdk listeners pisexcompat registered.
			pisex::Shutdown();
		} catch (const std::exception& ex) {
			LogError(std::string("as_status: OnPluginEnd failed: ") + ex.what());
		} catch (...) {
			LogError("as_status: OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

AsStatusPlugin g_asStatusPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asStatusPlugin)
