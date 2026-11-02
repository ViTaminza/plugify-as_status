// as_status — Plugify C++ port of the Pisex Metamod:Source module "[AS] Status" (as_status.cpp).
//
// Behaviour kept from the original:
//   * console command "mm_status": the server console (-1) always gets the player list on the
//     server console; a player who reaches the handler needs "@admin/status" and gets the list
//     on their own console;
//   * chat command "!status": players with "@admin/status" get the list in chat;
//   * the list is the original one: for every slot 0..63 that is not a bot,
//     "ID. Name - SteamID64"; a slot without a live client contributes an empty name and
//     steamid 0, which is kept (the original did not check the connection state at all);
//   * no phrases: the original printed literal English text.
//
// Pure s2sdk, no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7): the guards the other
// modules get from libs/s2guard live in this file.
//   * PITFALLS B1/B2 - the map gate g_mapReady. s2sdk client functions dereference null before
//     the first map and, worse, an emptied client list between OnMapEnd and the next OnMapStart.
//     The map listeners are registered in OnPluginStart; readiness is latched by OnMapStart and
//     by the first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd was
//     seen since OnPluginStart), and dropped by OnMapEnd. No client call is made while closed.
//   * PITFALLS C2 - every client query goes through CanQueryClient/HasClient: slot 0..63,
//     IsClientConnected && IsClientInGame && !IsClientSourceTV, and only then IsFakeClient /
//     GetClientName / GetClientSteamID64. s2sdk's "<unknown>" name becomes "", the SteamID64 of
//     an invalid or bot slot is 0.
//   * PITFALLS C11 - "mm_status" is a console command of this plugin; "status" is the engine's
//     own command, so the plugin only attaches a *listener* to it (the chat spelling "!status")
//     and never registers, replaces or removes "status" itself. OnPluginEnd removes "mm_status"
//     with RemoveCommand only when AddConsoleCommand created it; a name that already existed is
//     detached with RemoveCommandListener, exactly like "status".
//
// API notes (s2sdk 3.0.0):
//   * there is no CommandExists(): s2sdk::AddConsoleCommand() answers false when the name already
//     existed and then only hooks that command, so that bool is the "created by us" bit kept here;
//   * there is no GetClientXuid() export: GetClientSteamID64() is the id the original printed;
//   * PrintToServer() is ConMsg("%s\n"), so a line already ending in '\n' is passed without that
//     last newline to keep one output line per list entry (the original's META_CONPRINTF did).
//
// Nothing touches a client in OnPluginStart (commands are only dispatched with a live map);
// OnPluginStart never returns Failed - that would stop every Plugify plugin on the server.

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <admin_api.hpp>

namespace {

// CS2 has 64 player slots; the original iterated 0..63 without asking for a maximum.
constexpr int32_t kMaxSlots = 64;

admin_system::AdminApi* g_pAdminApi = nullptr;

// --- PITFALLS B1/B2: the map gate ------------------------------------------------

bool g_mapReady = false;           // a map is up: s2sdk client/entity calls are safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided
bool g_mapListeners = false;       // OnMapStart/OnMapEnd are registered with s2sdk

// --- PITFALLS C11: command bookkeeping -------------------------------------------

bool g_mmStatusRegistered = false; // "mm_status" is hooked (created here or pre-existing)
bool g_mmStatusCreated = false;    // AddConsoleCommand returned true: the name is ours to remove
bool g_statusListening = false;    // the "!status" listener is attached to the engine command

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

// s2sdk listener callbacks are plain function pointers called on the game thread. They must not
// throw, and they must not touch a client: OnMapEnd only clears state (PITFALLS B2).

void OnMapStartListener() {
	// From here on the clients and entities of the new map are in place.
	g_mapReady = true;
}

void OnMapEndListener() {
	// From here until the next map start every s2sdk client query can crash (PITFALLS B2).
	g_mapReady = false;
	g_mapEndedSinceStart = true;
}

// --- PITFALLS C2: the client guards ------------------------------------------------

// The map gate and the slot range are the only requirements for a function that addresses a
// client without reading its state (PrintToConsole).
bool CanQueryClient(int32_t iSlot) {
	return g_mapReady && iSlot >= 0 && iSlot < kMaxSlots;
}

// A live, human-addressable slot: connected, in game and not the SourceTV proxy. Only a slot that
// passes this may be handed to IsFakeClient / GetClientName / GetClientSteamID64 (PITFALLS C2).
bool HasClient(int32_t iSlot) {
	if (!CanQueryClient(iSlot) || !__s2sdk_IsClientConnected || !__s2sdk_IsClientInGame ||
		!__s2sdk_IsClientSourceTV) {
		return false;
	}
	return s2sdk::IsClientConnected(iSlot) && s2sdk::IsClientInGame(iSlot) &&
		   !s2sdk::IsClientSourceTV(iSlot);
}

bool IsBot(int32_t iSlot) {
	return HasClient(iSlot) && __s2sdk_IsFakeClient && s2sdk::IsFakeClient(iSlot);
}

// A connected, in-game human (bots and the SourceTV proxy are excluded).
bool IsHumanClient(int32_t iSlot) {
	return HasClient(iSlot) && !IsBot(iSlot);
}

std::string ClientName(int32_t iSlot) {
	if (!HasClient(iSlot) || !__s2sdk_GetClientName) {
		return {};
	}
	const plg::string szName = s2sdk::GetClientName(iSlot);
	std::string out(szName.data(), szName.size());
	if (out == "<unknown>") {
		// s2sdk's placeholder for a name that could not be resolved: the original printed "".
		out.clear();
	}
	return out;
}

// The SteamID64 the original printed. s2sdk 3.0.0 has no GetClientXuid() export, and a bot has no
// net channel and no identity (PITFALLS B5), so only a valid non-bot client has a non-zero id.
uint64_t ClientXuid(int32_t iSlot) {
	if (!IsHumanClient(iSlot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(iSlot);
}

// --- the output helpers ------------------------------------------------------------

void PrintToConsole(int32_t iSlot, std::string_view text) {
	if (iSlot < 0) {
		// The server console / RCON: not a client, reachable even while no map is up.
		if (!__s2sdk_PrintToServer) {
			return;
		}
		// PrintToServer is ConMsg("%s\n"): drop one trailing newline so a "line\n" stays one line.
		if (!text.empty() && text.back() == '\n') {
			text.remove_suffix(1);
		}
		s2sdk::PrintToServer(plg::string(text));
		return;
	}
	if (!CanQueryClient(iSlot) || !__s2sdk_PrintToConsole) {
		return;
	}
	s2sdk::PrintToConsole(iSlot, plg::string(text));
}

void PrintToChat(int32_t iSlot, std::string_view text) {
	if (iSlot < 0) {
		if (!__s2sdk_PrintToServer) {
			return;
		}
		if (!text.empty() && text.back() == '\n') {
			text.remove_suffix(1);
		}
		s2sdk::PrintToServer(plg::string(text));
		return;
	}
	if (!HasClient(iSlot) || !__s2sdk_PrintToChat) {
		return;
	}
	s2sdk::PrintToChat(iSlot, plg::string(text));
}

// The original's Get_Status(): the same lines, only the output destination depends on bConsole
// (print to the slot's console vs. the slot's chat).
void GetStatus(int32_t iSlot, bool bConsole) {
	if (bConsole) {
		PrintToConsole(iSlot, "ID. Name - SteamID\n");
	} else {
		PrintToChat(iSlot, "ID. Name - SteamID\n");
	}
	for (int32_t i = 0; i < kMaxSlots; ++i) {
		if (IsBot(i)) {
			continue;
		}
		const std::string szName = ClientName(i);
		const std::string szXuid = std::to_string(ClientXuid(i));
		const std::string szLine = std::to_string(i) + ". " + szName + " - " + szXuid + "\n";
		if (bConsole) {
			PrintToConsole(iSlot, szLine);
		} else {
			PrintToChat(iSlot, szLine);
		}
	}
}

// --- the commands -------------------------------------------------------------------

// "mm_status": console command. Chat "!mm_status" reaches this same callback through s2sdk's chat
// path, which looks a command up by name regardless of its flags; it is not this console command,
// so it is left to s2sdk (Continue). A caller that is not the server console needs the original's
// "@admin/status"; a refusal stays silent, exactly like the original's `return true`.
s2sdk::ResultType OnMmStatusCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
									const plg::vector<plg::string>& /*args*/) {
	try {
		if (eContext == s2sdk::ConCommandContext::Chat) {
			return s2sdk::ResultType::Continue;
		}
		if (iSlot == -1 ||
			(g_pAdminApi != nullptr && g_pAdminApi->HasPermission(iSlot, "@admin/status"))) {
			GetStatus(iSlot, /*bConsole=*/true); // the server console or the caller's own console
		}
		return s2sdk::ResultType::Handled;
	} catch (const std::exception& ex) {
		LogError(std::string("as_status: mm_status failed: ") + ex.what());
	} catch (...) {
		LogError("as_status: mm_status failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// "!status": a listener on the engine's own "status". Only the chat spelling is ours - a console
// "status" must keep running the engine command untouched (PITFALLS C11), hence Continue.
s2sdk::ResultType OnStatusCommand(int32_t iSlot, s2sdk::ConCommandContext eContext,
								  const plg::vector<plg::string>& /*args*/) {
	if (eContext != s2sdk::ConCommandContext::Chat) {
		return s2sdk::ResultType::Continue;
	}
	try {
		if (g_pAdminApi != nullptr && g_pAdminApi->HasPermission(iSlot, "@admin/status")) {
			GetStatus(iSlot, /*bConsole=*/false);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("as_status: !status failed: ") + ex.what());
	} catch (...) {
		LogError("as_status: !status failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

class AsStatusPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			g_pAdminApi = &admin_system::AdminApi::Get();
			g_mapReady = false;
			g_mapEndedSinceStart = false;
			g_firstUpdateDone = false;

			// PITFALLS B1: registering the map listeners touches no client, so it is safe here.
			// Without the listeners nothing could ever close the gate again, so a missing binding
			// keeps it shut (CanQueryClient stays false) instead of opening it blindly.
			if (!g_mapListeners && __s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
				s2sdk::OnMapStart_Register(&OnMapStartListener);
				s2sdk::OnMapEnd_Register(&OnMapEndListener);
				g_mapListeners = true;
			} else if (!__s2sdk_OnMapStart_Register || !__s2sdk_OnMapEnd_Register) {
				LogError("as_status: the s2sdk map listeners are unavailable - client calls stay disabled");
			}

			// "mm_status": a player with "@admin/status" may run it from their own console (the
			// list then goes to that console), and the server console (-1) always gets it.
			// ClientCanExecute|Release lets a client console reach the handler so the permission
			// check can run, like the Pisex RegCommand this replaces (PITFALLS C11).
			// false = the name already existed (engine or another plugin) and s2sdk only attached
			// a listener to it: OnPluginEnd must then detach the listener and never RemoveCommand
			// a command this plugin did not create (PITFALLS C11).
			if (!g_mmStatusRegistered && __s2sdk_AddConsoleCommand) {
				const auto mmFlags = static_cast<s2sdk::ConVarFlag>(
					static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
					static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
				g_mmStatusCreated = s2sdk::AddConsoleCommand("mm_status", "", mmFlags,
															 &OnMmStatusCommand, s2sdk::HookMode::Pre);
				g_mmStatusRegistered = true;
			} else if (!__s2sdk_AddConsoleCommand) {
				LogError("as_status: s2sdk AddConsoleCommand is unavailable - 'mm_status' is not registered");
			}

			// "!status": only a listener on the engine's "status", never a registration of it.
			if (!g_statusListening && __s2sdk_AddCommandListener) {
				g_statusListening =
					s2sdk::AddCommandListener("status", &OnStatusCommand, s2sdk::HookMode::Pre);
				if (!g_statusListening) {
					LogError("as_status: could not listen on the engine's 'status' command");
				}
			} else if (!__s2sdk_AddCommandListener) {
				LogError("as_status: s2sdk AddCommandListener is unavailable - '!status' is disabled");
			}

			LogMessage("as_status: ready ('mm_status' and '!status' registered)", plg::Severity::Info);
		} catch (const std::exception& ex) {
			LogError(std::string("as_status: OnPluginStart failed: ") + ex.what());
		} catch (...) {
			LogError("as_status: OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		// A plugin loaded into a running map never saw OnMapStart: its first update is the "a map
		// is up" signal (PITFALLS B1). An OnMapEnd seen since OnPluginStart means the server is
		// between maps, so the latch waits for the next OnMapStart instead.
		if (!g_firstUpdateDone) {
			g_firstUpdateDone = true;
			if (g_mapListeners && !g_mapEndedSinceStart) {
				g_mapReady = true;
			}
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			// "status" is the engine's command: detach the chat listener only. RemoveCommand would
			// unregister the engine command itself (PITFALLS C11) and is never called for it.
			if (g_statusListening && __s2sdk_RemoveCommandListener) {
				s2sdk::RemoveCommandListener("status", &OnStatusCommand, s2sdk::HookMode::Pre);
			}
			g_statusListening = false;

			if (g_mmStatusRegistered) {
				if (g_mmStatusCreated) {
					if (__s2sdk_RemoveCommand) {
						s2sdk::RemoveCommand("mm_status", &OnMmStatusCommand);
					}
				} else if (__s2sdk_RemoveCommandListener) {
					// The name existed before this plugin: detach our listener, leave the command.
					s2sdk::RemoveCommandListener("mm_status", &OnMmStatusCommand, s2sdk::HookMode::Pre);
				}
			}
			g_mmStatusRegistered = false;
			g_mmStatusCreated = false;

			if (g_mapListeners) {
				if (__s2sdk_OnMapStart_Unregister) {
					s2sdk::OnMapStart_Unregister(&OnMapStartListener);
				}
				if (__s2sdk_OnMapEnd_Unregister) {
					s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
				}
				g_mapListeners = false;
			}

			// A stale callback (the engine still holds the pointer for a moment) can no longer
			// reach a client or an admin check.
			g_mapReady = false;
			g_mapEndedSinceStart = true;
			g_firstUpdateDone = false;
			g_pAdminApi = nullptr;
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
