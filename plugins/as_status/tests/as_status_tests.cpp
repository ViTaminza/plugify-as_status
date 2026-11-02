// as_status tests — no engine: the plugin TU is compiled in (its handlers live in an anonymous
// namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake.
//
// Covered:
//   * PITFALLS B1/B2 — the map gate: not one s2sdk client call before the first map, between
//     OnMapEnd and the next OnMapStart, or when the plugin was loaded into the map-change window
//     (OnMapEnd before the first update must not latch readiness);
//   * PITFALLS C2 — slot range, connected/in-game/SourceTV checks before IsFakeClient /
//     GetClientName / GetClientSteamID64, "<unknown>" -> "", a bot's SteamID64 is 0;
//   * PITFALLS C11 — "mm_status" from the server console (no permission), from a permitted and an
//     unpermitted client, chat "!mm_status" left alone, chat "!status" vs the console "status"
//     passthrough;
//   * a clean unload: RemoveCommand only for the name this plugin created, RemoveCommandListener
//     for the engine's own "status", and no client call from a stale callback after OnPluginEnd.
//
// Run by ./test.sh (build/tests/as_status_tests).

#include "../src/plugin.cpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

bool HasFlag(s2sdk::ConVarFlag flags, s2sdk::ConVarFlag flag) {
	return (static_cast<int64_t>(flags) & static_cast<int64_t>(flag)) != 0;
}

plg::vector<plg::string> Args(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

// --- the fake map lifecycle ----------------------------------------------------------------

s2sdk::OnMapStartCallback g_onMapStart = nullptr;
s2sdk::OnMapEndCallback g_onMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_onMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_onMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

void MapStart() {
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void MapEnd() {
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// --- the fake client layer -----------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	std::string name;
	uint64_t steamid64 = 0;
};

struct FakePrint {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakeClient> g_clients(64);
int g_clientCalls = 0; // every fake below that is a client query/output counts into this
int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTvCalls = 0;
int g_fakeCalls = 0;
int g_nameCalls = 0;
int g_steamIdCalls = 0;
std::vector<FakePrint> g_chatPrints;
std::vector<FakePrint> g_consolePrints;
std::vector<std::string> g_serverPrints;

const FakeClient& Slot(int32_t slot) {
	static const FakeClient empty;
	return slot >= 0 && slot < static_cast<int32_t>(g_clients.size())
			   ? g_clients[static_cast<std::size_t>(slot)]
			   : empty;
}

void ResetClientCounters() {
	g_clientCalls = 0;
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTvCalls = 0;
	g_fakeCalls = 0;
	g_nameCalls = 0;
	g_steamIdCalls = 0;
}

void ResetPrints() {
	g_chatPrints.clear();
	g_consolePrints.clear();
	g_serverPrints.clear();
}

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	++g_connectedCalls;
	return Slot(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	++g_inGameCalls;
	return Slot(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	++g_sourceTvCalls;
	return Slot(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_clientCalls;
	++g_fakeCalls;
	return Slot(slot).fake;
}

plg::string FakeGetClientName(int32_t slot) {
	++g_clientCalls;
	++g_nameCalls;
	// s2sdk answers "<unknown>" when it cannot resolve the client name.
	return plg::string(Slot(slot).name.empty() ? "<unknown>" : Slot(slot).name);
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_clientCalls;
	++g_steamIdCalls;
	return Slot(slot).steamid64;
}

void FakePrintToChat(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_chatPrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToConsole(int32_t slot, const plg::string& text) {
	++g_clientCalls;
	g_consolePrints.push_back(FakePrint{slot, ToStd(text)});
}

void FakePrintToServer(const plg::string& text) {
	// Not a client call: it must stay reachable even while no map is up.
	g_serverPrints.push_back(ToStd(text));
}

// --- the fake command manager ----------------------------------------------------------------

struct FakeCommand {
	std::string name;
	std::string description;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

struct FakeListener {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<FakeCommand> g_consoleCommands;
std::set<std::string> g_existingCommands; // AddConsoleCommand answers false for these
std::vector<FakeListener> g_addedListeners;
std::vector<FakeListener> g_removedListeners;
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_removedCommands;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& description,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode mode) {
	const std::string key(name.data(), name.size());
	g_consoleCommands.push_back(
		FakeCommand{key, ToStd(description), flags, callback, mode});
	// false = the name already exists (the engine's command or another plugin's); s2sdk then
	// only attaches the listener to that command.
	return g_existingCommands.count(key) == 0;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback callback) {
	g_removedCommands.emplace_back(std::string(name.data(), name.size()), callback);
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode mode) {
	g_addedListeners.push_back(FakeListener{std::string(name.data(), name.size()), callback, mode});
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							   s2sdk::HookMode mode) {
	g_removedListeners.push_back(FakeListener{std::string(name.data(), name.size()), callback, mode});
	return true;
}

const FakeCommand* FindCommand(const std::string& name) {
	for (const FakeCommand& command : g_consoleCommands) {
		if (command.name == name) {
			return &command;
		}
	}
	return nullptr;
}

int CountCommands(const std::string& name) {
	int count = 0;
	for (const FakeCommand& command : g_consoleCommands) {
		count += command.name == name ? 1 : 0;
	}
	return count;
}

const FakeListener* FindListener(const std::string& name) {
	for (const FakeListener& listener : g_addedListeners) {
		if (listener.name == name) {
			return &listener;
		}
	}
	return nullptr;
}

bool RemovedListener(const std::string& name, s2sdk::ConCommandCallback callback,
					 s2sdk::HookMode mode) {
	for (const FakeListener& listener : g_removedListeners) {
		if (listener.name == name && listener.callback == callback && listener.mode == mode) {
			return true;
		}
	}
	return false;
}

bool RemovedCommand(const std::string& name) {
	for (const auto& [removedName, callback] : g_removedCommands) {
		(void)callback;
		if (removedName == name) {
			return true;
		}
	}
	return false;
}

bool RemovedCommand(const std::string& name, s2sdk::ConCommandCallback callback) {
	for (const auto& [removedName, removedCallback] : g_removedCommands) {
		if (removedName == name && removedCallback == callback) {
			return true;
		}
	}
	return false;
}

// s2sdk's execute path: the recorded callback is called with the engine's tokenised arguments.
s2sdk::ResultType CallCommand(s2sdk::ConCommandCallback callback, int32_t caller,
							  s2sdk::ConCommandContext context,
							  std::initializer_list<const char*> args) {
	const plg::vector<plg::string> argv = Args(args);
	return callback(caller, context, argv);
}

s2sdk::ResultType DispatchCommand(const std::string& name, int32_t caller,
								  s2sdk::ConCommandContext context,
								  std::initializer_list<const char*> args) {
	const FakeCommand* command = FindCommand(name);
	Check(command != nullptr, "command \"" + name + "\" is registered with s2sdk");
	if (command == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return CallCommand(command->callback, caller, context, args);
}

s2sdk::ResultType DispatchListener(const std::string& name, int32_t caller,
								   s2sdk::ConCommandContext context,
								   std::initializer_list<const char*> args) {
	const FakeListener* listener = FindListener(name);
	Check(listener != nullptr, "listener \"" + name + "\" is installed");
	if (listener == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return CallCommand(listener->callback, caller, context, args);
}

// --- the fake admin_system bindings -----------------------------------------------------------

std::vector<std::pair<int32_t, std::string>> g_permissionChecks;
std::set<int32_t> g_permittedSlots;

float FakeGetPluginVersion() {
	return 1.0f;
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	g_permissionChecks.emplace_back(slot, ToStd(permission));
	return g_permittedSlots.count(slot) != 0;
}

// --- the recording helpers ---------------------------------------------------------------------

bool HasServerPrint(const std::string& needle) {
	for (const std::string& line : g_serverPrints) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasConsolePrint(const std::string& needle) {
	for (const FakePrint& print : g_consolePrints) {
		if (print.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasChatPrint(const std::string& needle) {
	for (const FakePrint& print : g_chatPrints) {
		if (print.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool AllConsolePrintsAt(int32_t slot) {
	if (g_consolePrints.empty()) {
		return false;
	}
	for (const FakePrint& print : g_consolePrints) {
		if (print.slot != slot) {
			return false;
		}
	}
	return true;
}

bool AllChatPrintsAt(int32_t slot) {
	if (g_chatPrints.empty()) {
		return false;
	}
	for (const FakePrint& print : g_chatPrints) {
		if (print.slot != slot) {
			return false;
		}
	}
	return true;
}

void InstallFakes() {
	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToConsole = &FakePrintToConsole;
	__s2sdk_PrintToServer = &FakePrintToServer;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_HasPermission = &FakeHasPermission;
}

// Forgets every fake registration and all recorded state. The plugin must have been stopped first
// (PrepareTest/FinishTest do that), otherwise it still believes its commands are registered.
void ResetWorld() {
	g_clients.assign(64, FakeClient{});
	g_permittedSlots.clear();
	g_permissionChecks.clear();
	ResetClientCounters();
	ResetPrints();

	g_consoleCommands.clear();
	g_existingCommands.clear();
	g_addedListeners.clear();
	g_removedListeners.clear();
	g_removedCommands.clear();

	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
}

// Stops whatever a previous test left behind (OnPluginEnd is a no-op when nothing is registered)
// and starts the next test from an empty fake world.
void PrepareTest() {
	g_asStatusPlugin.OnPluginEnd();
	ResetWorld();
}

void FinishTest() {
	g_asStatusPlugin.OnPluginEnd();
	ResetWorld();
}

// --- A. the map gate (PITFALLS B1/B2) ----------------------------------------------------------

void TestMapGate() {
	PrepareTest();
	Check(g_asStatusPlugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart returns Ok (never Failed)");
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1,
		  "OnPluginStart registers both map listeners");
	Check(!g_mapReady, "the gate is shut on plugin start");

	g_clients[3] = FakeClient{true, true, false, false, "Alice", 76561198000000003ULL};

	// --- before the first map: not one client call may reach s2sdk ----------------------------
	ResetClientCounters();
	Check(!CanQueryClient(3), "CanQueryClient is false before the gate opens");
	Check(!HasClient(3), "HasClient is false before the gate opens");
	Check(!IsBot(3), "IsBot is false before the gate opens");
	Check(ClientName(3).empty(), "ClientName is empty before the gate opens");
	Check(ClientXuid(3) == 0, "ClientXuid is 0 before the gate opens");
	PrintToConsole(3, "x");
	PrintToChat(3, "x");
	Check(g_clientCalls == 0, "not one s2sdk client call is made before the gate opens");
	Check(g_consolePrints.empty() && g_chatPrints.empty() && g_serverPrints.empty(),
		  "no client-directed output before the gate opens");

	// The server console is not a client: it stays reachable even while no map is up.
	PrintToConsole(-1, "line\n");
	Check(g_serverPrints.size() == 1 && g_serverPrints[0] == "line",
		  "the server console gets the line without the redundant newline");
	Check(g_clientCalls == 0, "the server console output is not counted as a client call");
	ResetPrints();

	// mm_status in the same window: the header reaches the server console, no client is queried.
	ResetClientCounters();
	Check(DispatchCommand("mm_status", -1, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "the server console command is handled");
	Check(g_clientCalls == 0, "mm_status before the gate opens makes no client call");
	Check(HasServerPrint("ID. Name - SteamID"), "the header still reaches the server console");

	// A plugin loaded into a running map: its first update latches readiness.
	Check(!g_mapReady, "the map start listener was not called yet");
	g_asStatusPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(g_mapReady, "the first OnPluginUpdate latches the gate");
	ResetClientCounters();
	Check(HasClient(3), "a live client is reachable after the latch");
	Check(ClientName(3) == "Alice", "the client name is read after the latch");
	Check(ClientXuid(3) == 76561198000000003ULL, "the SteamID64 is read after the latch");
	Check(!IsBot(3), "a human is not a bot");
	Check(g_clientCalls > 0, "the client calls reach s2sdk after the latch");

	// --- after OnMapEnd: not one client call may reach s2sdk ---------------------------------
	MapEnd();
	Check(!g_mapReady, "OnMapEnd drops readiness");
	g_asStatusPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "an update after OnMapEnd does not re-open the gate");
	ResetClientCounters();
	Check(!HasClient(3), "HasClient is false after OnMapEnd");
	Check(ClientName(3).empty(), "ClientName is empty after OnMapEnd");
	Check(ClientXuid(3) == 0, "ClientXuid is 0 after OnMapEnd");
	PrintToConsole(3, "x");
	PrintToChat(3, "x");
	Check(g_clientCalls == 0, "not one s2sdk client call is made after OnMapEnd");
	Check(g_consolePrints.empty() && g_chatPrints.empty(),
		  "no client-directed output after OnMapEnd");

	ResetPrints();
	ResetClientCounters();
	Check(DispatchCommand("mm_status", -1, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "the command is still handled in the map-change window");
	Check(g_clientCalls == 0, "mm_status in the map-change window makes no client call");
	Check(HasServerPrint("ID. Name - SteamID") && HasServerPrint("0.  - 0"),
		  "the map-change list is the header plus empty lines, exactly like the original");

	// --- the next OnMapStart opens the gate again without waiting for an update ---------------
	MapStart();
	Check(g_mapReady, "OnMapStart latches the gate");
	Check(HasClient(3) && ClientName(3) == "Alice", "the client is reachable again");

	FinishTest();
}

// A plugin loaded during the map change (OnMapEnd before its first update) must not latch on the
// update: the first update only counts when no OnMapEnd was seen since OnPluginStart.
void TestMapChangeWindowStart() {
	PrepareTest();
	g_asStatusPlugin.OnPluginStart();
	MapEnd();
	g_asStatusPlugin.OnPluginUpdate(std::chrono::milliseconds(0));
	Check(!g_mapReady, "the first update after an OnMapEnd does not latch the gate");

	g_clients[2] = FakeClient{true, true, false, false, "Bob", 76561198000000002ULL};
	ResetClientCounters();
	Check(ClientXuid(2) == 0, "no client id in the map-change window");
	Check(g_clientCalls == 0, "no client call in the map-change window");

	MapStart();
	Check(g_mapReady, "the next OnMapStart opens the gate");
	Check(HasClient(2) && ClientName(2) == "Bob", "the client is reachable after the map start");

	FinishTest();
}

// --- B. the client guards (PITFALLS C2) -------------------------------------------------------

void TestClientGuards() {
	PrepareTest();
	g_asStatusPlugin.OnPluginStart();
	MapStart();
	Check(g_mapReady, "the map gate is open");

	g_clients[7].connected = true;                 // connected, not in game
	g_clients[7].steamid64 = 76561198000000007ULL; // stale data of the previous occupant
	g_clients[8] = FakeClient{true, true, false, false, "", 76561198000000008ULL}; // "<unknown>" name
	g_clients[9] = FakeClient{true, true, true, false, "SourceTV", 0};              // the proxy
	g_clients[10] = FakeClient{true, true, false, true, "BOT Rick", 76561198000000010ULL}; // a bot
	g_clients[11] = FakeClient{true, true, false, false, "Carol", 76561198000000011ULL};

	// --- out-of-range slots never reach s2sdk ------------------------------------------------
	ResetClientCounters();
	Check(!CanQueryClient(-1) && !CanQueryClient(64), "slots -1 and 64 cannot be queried");
	Check(!HasClient(-1) && !HasClient(64), "out-of-range slots have no client");
	Check(!IsBot(-1) && !IsBot(64), "out-of-range slots are not bots");
	Check(ClientName(-1).empty() && ClientName(64).empty(), "out-of-range slots have no name");
	Check(ClientXuid(-1) == 0 && ClientXuid(64) == 0, "out-of-range slots have no SteamID64");
	PrintToConsole(64, "x");
	PrintToChat(64, "x");
	Check(g_clientCalls == 0, "slots -1 and 64 never reach a client binding");
	Check(g_consolePrints.empty() && g_chatPrints.empty(), "slots -1 and 64 get no output");
	ResetPrints();

	// --- a connected slot that is not in game: no name/id query ------------------------------
	ResetClientCounters();
	Check(!HasClient(7), "a connected slot that is not in game has no client");
	Check(ClientName(7).empty(), "no name for a slot that is not in game");
	Check(ClientXuid(7) == 0, "a stale SteamID64 of a former occupant is not read");
	Check(g_nameCalls == 0 && g_steamIdCalls == 0,
		  "GetClientName/GetClientSteamID64 are not called");
	Check(g_fakeCalls == 0,
		  "IsFakeClient is not called before connected/in-game/SourceTV all passed");

	// --- s2sdk's "<unknown>" placeholder becomes "" -------------------------------------------
	ResetClientCounters();
	Check(HasClient(8), "a connected in-game human has a client");
	Check(ClientName(8).empty(), "s2sdk's \"<unknown>\" placeholder becomes an empty name");
	Check(g_nameCalls == 1, "GetClientName was called once");

	// --- the SourceTV proxy is not a client ---------------------------------------------------
	ResetClientCounters();
	Check(!HasClient(9), "the SourceTV proxy is not a client");
	Check(!IsBot(9), "the SourceTV proxy is not queried as a bot");
	Check(g_fakeCalls == 0, "IsFakeClient is not called for the SourceTV proxy");
	Check(ClientXuid(9) == 0, "the SourceTV proxy has no SteamID64");

	// --- a bot has no identity, a human has one -----------------------------------------------
	ResetClientCounters();
	Check(IsBot(10), "a fake client is a bot");
	Check(ClientXuid(10) == 0, "a bot answers SteamID64 0 even when the engine reports one");
	Check(g_steamIdCalls == 0, "GetClientSteamID64 is never called for a bot");
	Check(!IsBot(11), "a human is not a bot");
	Check(ClientXuid(11) == 76561198000000011ULL, "a human's SteamID64 is read");

	// --- output routing -----------------------------------------------------------------------
	ResetPrints();
	PrintToConsole(-1, "a\n");
	Check(g_serverPrints.size() == 1 && g_serverPrints[0] == "a",
		  "slot -1 goes to the server console as one line");
	PrintToChat(-1, "b\n");
	Check(g_serverPrints.size() == 2 && g_serverPrints[1] == "b",
		  "chat to slot -1 also goes to the server console");
	PrintToConsole(11, "c");
	Check(g_consolePrints.size() == 1 && g_consolePrints[0].slot == 11 &&
			  g_consolePrints[0].text == "c",
		  "a console line goes to that client's console unchanged");
	PrintToChat(11, "d");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].slot == 11 && g_chatPrints[0].text == "d",
		  "a chat line goes to that client's chat");
	PrintToChat(7, "e"); // not in game: no chat output
	Check(g_chatPrints.size() == 1, "a chat line is not sent to a slot that is not in game");

	FinishTest();
}

// --- C. mm_status (PITFALLS C11) --------------------------------------------------------------

void TestMmStatusCommand() {
	PrepareTest();
	g_asStatusPlugin.OnPluginStart();
	MapStart();
	Check(g_mapReady, "the map gate is open");

	g_clients[3] = FakeClient{true, true, false, false, "Alice", 76561198000000003ULL};
	g_clients[4] = FakeClient{true, true, false, false, "Bob", 76561198000000004ULL};
	g_clients[5] = FakeClient{true, true, false, true, "BOT Rick", 0};
	g_permittedSlots = {3};

	const FakeCommand* command = FindCommand("mm_status");
	Check(command != nullptr, "mm_status is registered with s2sdk");
	if (command == nullptr) {
		FinishTest();
		return;
	}
	Check(HasFlag(command->flags, s2sdk::ConVarFlag::Release) &&
			  HasFlag(command->flags, s2sdk::ConVarFlag::ClientCanExecute),
		  "mm_status is registered with ConVarFlag::Release | ConVarFlag::ClientCanExecute");
	Check(command->mode == s2sdk::HookMode::Pre, "mm_status is hooked in Pre mode");
	Check(command->callback == &OnMmStatusCommand, "mm_status runs this plugin's handler");
	Check(g_mmStatusCreated, "AddConsoleCommand(true) records that the name is ours");
	Check(CountCommands("status") == 0,
		  "the engine's status is never registered as a console command");
	Check(FindListener("status") != nullptr && CountCommands("status") == 0,
		  "status only gets a listener, never a registration");

	// --- the server console: no permission, the list stays on the server console -------------
	ResetPrints();
	ResetClientCounters();
	Check(DispatchCommand("mm_status", -1, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "the server console call is handled");
	Check(g_permissionChecks.empty(), "the server console needs no @admin/status");
	Check(HasServerPrint("ID. Name - SteamID"), "the header reaches the server console");
	Check(HasServerPrint("3. Alice - 76561198000000003"),
		  "a human line reaches the server console");
	Check(HasServerPrint("4. Bob - 76561198000000004"), "every human is listed");
	Check(!HasServerPrint("BOT Rick"), "the bot is skipped");
	Check(HasServerPrint("0.  - 0"), "an empty slot keeps the original's empty line");
	Check(g_serverPrints.size() == 64, "the header plus one line per non-bot slot");
	Check(g_consolePrints.empty() && g_chatPrints.empty(),
		  "the server console list is not sent through a client print");
	Check(g_clientCalls > 0, "the listing queried the clients");

	// --- a permitted client gets the list on its own console ---------------------------------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchCommand("mm_status", 3, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "a permitted client call is handled");
	Check(g_permissionChecks.size() == 1 && g_permissionChecks[0].first == 3 &&
			  g_permissionChecks[0].second == "@admin/status",
		  "a client caller is checked for @admin/status");
	Check(AllConsolePrintsAt(3), "the list goes to the caller's console");
	Check(g_consolePrints.size() == 64, "the header plus one line per non-bot slot");
	Check(HasConsolePrint("3. Alice - 76561198000000003"), "the list contains the clients");
	Check(g_serverPrints.empty(), "a client's list does not go to the server console");
	Check(g_chatPrints.empty(), "a console list does not go to chat");

	// --- an unpermitted client gets nothing, the command is still consumed -------------------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchCommand("mm_status", 4, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "an unpermitted client call is handled and swallowed");
	Check(g_permissionChecks.size() == 1 && g_permissionChecks[0].first == 4,
		  "the unpermitted caller was checked");
	Check(g_consolePrints.empty() && g_chatPrints.empty() && g_serverPrints.empty(),
		  "an unpermitted client gets no output");

	// --- chat "!mm_status" is not this console command --------------------------------------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchCommand("mm_status", 3, s2sdk::ConCommandContext::Chat, {"mm_status"}) ==
			  s2sdk::ResultType::Continue,
		  "chat !mm_status is left to s2sdk (Continue)");
	Check(g_permissionChecks.empty(), "the chat spelling is not treated as a console command");
	Check(g_consolePrints.empty() && g_chatPrints.empty() && g_serverPrints.empty(),
		  "chat !mm_status produces no output");

	FinishTest();
}

// --- D. !status vs the engine's status (PITFALLS C11) -----------------------------------------

void TestStatusChatCommand() {
	PrepareTest();
	g_asStatusPlugin.OnPluginStart();
	MapStart();
	Check(g_mapReady, "the map gate is open");

	g_clients[3] = FakeClient{true, true, false, false, "Alice", 76561198000000003ULL};
	g_clients[4] = FakeClient{true, true, false, false, "Bob", 76561198000000004ULL};
	g_permittedSlots = {3};

	const FakeListener* listener = FindListener("status");
	Check(listener != nullptr, "the chat listener on the engine's status is installed");
	if (listener == nullptr) {
		FinishTest();
		return;
	}
	Check(listener->mode == s2sdk::HookMode::Pre, "the status listener is Pre");
	Check(listener->callback == &OnStatusCommand, "the status listener runs this plugin's handler");
	Check(CountCommands("status") == 0, "status is never registered by this plugin");

	// --- "!status" in chat for a permitted player --------------------------------------------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchListener("status", 3, s2sdk::ConCommandContext::Chat, {"status"}) ==
			  s2sdk::ResultType::Handled,
		  "chat !status is handled");
	Check(g_permissionChecks.size() == 1 && g_permissionChecks[0].first == 3 &&
			  g_permissionChecks[0].second == "@admin/status",
		  "the chat caller is checked for @admin/status");
	Check(AllChatPrintsAt(3), "the list goes to the caller's chat");
	Check(g_chatPrints.size() == 65, "the header plus one line per non-bot slot goes to chat");
	Check(HasChatPrint("3. Alice - 76561198000000003"), "the chat list contains the clients");
	Check(g_consolePrints.empty() && g_serverPrints.empty(), "the chat list goes nowhere else");

	// --- "!status" from a player without the permission: silent ------------------------------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchListener("status", 4, s2sdk::ConCommandContext::Chat, {"status"}) ==
			  s2sdk::ResultType::Handled,
		  "an unpermitted chat call is handled and swallowed");
	Check(g_chatPrints.empty() && g_consolePrints.empty() && g_serverPrints.empty(),
		  "an unpermitted chat caller gets nothing");

	// --- the console "status" is the engine's: the listener passes it through untouched ------
	ResetPrints();
	g_permissionChecks.clear();
	Check(DispatchListener("status", 3, s2sdk::ConCommandContext::Console, {"status"}) ==
			  s2sdk::ResultType::Continue,
		  "a client console status continues to the engine");
	Check(DispatchListener("status", -1, s2sdk::ConCommandContext::Console, {"status"}) ==
			  s2sdk::ResultType::Continue,
		  "the server console status continues to the engine");
	Check(g_permissionChecks.empty(), "the console status is not gated by @admin/status");
	Check(g_chatPrints.empty() && g_consolePrints.empty() && g_serverPrints.empty(),
		  "the console status produces no output from this plugin");

	FinishTest();
}

// --- E. a clean unload (PITFALLS A7/C11) -------------------------------------------------------

void TestUnload() {
	PrepareTest();
	g_asStatusPlugin.OnPluginStart();
	MapStart();
	g_clients[3] = FakeClient{true, true, false, false, "Alice", 76561198000000003ULL};

	const FakeCommand* command = FindCommand("mm_status");
	const FakeListener* listener = FindListener("status");
	Check(command != nullptr && listener != nullptr, "both registrations exist before the unload");
	if (command == nullptr || listener == nullptr) {
		FinishTest();
		return;
	}
	const s2sdk::ConCommandCallback mmCallback = command->callback;
	const s2sdk::ConCommandCallback statusCallback = listener->callback;

	g_asStatusPlugin.OnPluginEnd();
	Check(!g_mapReady, "OnPluginEnd drops readiness");
	Check(g_mapStartUnregisters == 1 && g_mapEndUnregisters == 1,
		  "OnPluginEnd unregisters both map listeners");
	Check(g_onMapStart == nullptr && g_onMapEnd == nullptr,
		  "the map callbacks are gone from s2sdk");
	Check(RemovedCommand("mm_status", mmCallback),
		  "an owned mm_status is removed with RemoveCommand");
	Check(!RemovedListener("mm_status", mmCallback, s2sdk::HookMode::Pre),
		  "an owned command is not detached as a listener");
	Check(RemovedListener("status", statusCallback, s2sdk::HookMode::Pre),
		  "the engine's status is detached with RemoveCommandListener");
	Check(!RemovedCommand("status"), "RemoveCommand is never called for the engine's status");

	// A stale callback (the engine may still hold the pointer for a moment) is harmless.
	ResetPrints();
	ResetClientCounters();
	Check(CallCommand(mmCallback, 3, s2sdk::ConCommandContext::Console, {"mm_status"}) ==
			  s2sdk::ResultType::Handled,
		  "the stale mm_status callback answers Handled");
	Check(CallCommand(statusCallback, 3, s2sdk::ConCommandContext::Chat, {"status"}) ==
			  s2sdk::ResultType::Handled,
		  "the stale status callback answers Handled");
	Check(g_clientCalls == 0, "a callback after OnPluginEnd makes no client call");
	Check(g_consolePrints.empty() && g_chatPrints.empty(),
		  "a callback after OnPluginEnd sends no client output");

	// plg reload: a fresh start after the end re-registers everything exactly once.
	g_asStatusPlugin.OnPluginStart();
	Check(g_mapStartRegisters == 2 && g_mapEndRegisters == 2,
		  "a reload registers the map listeners again");
	Check(FindCommand("mm_status") != nullptr, "a reload registers mm_status again");
	Check(FindListener("status") != nullptr, "a reload re-attaches the status listener");
	Check(!g_mapReady, "a reload starts with the gate shut");
	MapStart();
	Check(g_mapReady, "the reloaded plugin opens the gate on OnMapStart");

	FinishTest();
}

// A name that already existed must not be removed with RemoveCommand on unload: s2sdk only hooked
// it, and RemoveCommand would unregister somebody else's command.
void TestExistingMmStatusName() {
	PrepareTest();
	g_existingCommands.insert("mm_status");
	g_asStatusPlugin.OnPluginStart();

	const FakeCommand* command = FindCommand("mm_status");
	Check(command != nullptr && command->callback == &OnMmStatusCommand,
		  "an existing mm_status is hooked with this plugin's handler");
	Check(!g_mmStatusCreated, "AddConsoleCommand(false) records that the name is not ours");
	if (command == nullptr) {
		FinishTest();
		return;
	}
	const s2sdk::ConCommandCallback mmCallback = command->callback;

	g_asStatusPlugin.OnPluginEnd();
	Check(RemovedListener("mm_status", mmCallback, s2sdk::HookMode::Pre),
		  "an existing name is detached with RemoveCommandListener");
	Check(!RemovedCommand("mm_status"),
		  "RemoveCommand is not called for a command this plugin did not create");

	FinishTest();
}

} // namespace

int main() {
	std::printf("as_status tests (fake s2sdk/admin_system bindings, no engine)\n");

	InstallFakes();

	TestMapGate();
	TestMapChangeWindowStart();
	TestClientGuards();
	TestMmStatusCommand();
	TestStatusChatCommand();
	TestUnload();
	TestExistingMmStatusName();

	std::printf("as_status_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
