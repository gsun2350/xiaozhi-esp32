#include "music_mcp_tools.h"

#include <cJSON.h>
#include "mcp_server.h"
#include "music_player.h"

namespace {
const char* StateName(MusicState state) {
    switch (state) {
        case MusicState::Stopped:
            return "STOPPED";
        case MusicState::Playing:
            return "PLAYING";
        case MusicState::Paused:
            return "PAUSED";
        case MusicState::InterruptedByVoice:
            return "INTERRUPTED_BY_VOICE";
        case MusicState::Error:
            return "ERROR";
    }
    return "ERROR";
}

cJSON* MakeResult(const std::string& result) {
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "result", result.c_str());
    cJSON_AddBoolToObject(response, "accepted", result == "command_accepted");
    return response;
}

cJSON* MakeStatus() {
    MusicStatus status = MusicPlayer::GetInstance().GetStatus();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "song_id", status.song_id.c_str());
    cJSON_AddStringToObject(response, "state", StateName(status.state));
    cJSON_AddNumberToObject(response, "position_ms", status.position_ms);
    cJSON_AddBoolToObject(response, "card_mounted", status.card_mounted);
    cJSON_AddStringToObject(response, "last_error", status.error.c_str());
    return response;
}
}  // namespace

void RegisterMusicMcpTools() {
    auto& server = McpServer::GetInstance();
    server.AddTool("self.music.play",
                   "Play an MP3 from the TF card by a verified song_id. A returned "
                   "command_accepted means queued, not yet audible.",
                   PropertyList({Property("song_id", kPropertyTypeString).SetMaxLength(32)}),
                   [](const PropertyList& properties) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Play(
                           properties["song_id"].value<std::string>()));
                   });
    server.AddTool("self.music.pause", "Pause TF music and keep its position.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Pause());
                   });
    server.AddTool("self.music.resume", "Resume paused TF music.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Resume());
                   });
    server.AddTool("self.music.stop", "Stop TF music.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Stop());
                   });
    server.AddTool("self.music.next", "Play the next MP3 in /sdcard/music.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Next());
                   });
    server.AddTool("self.music.previous", "Play the previous MP3 in /sdcard/music.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue {
                       return MakeResult(MusicPlayer::GetInstance().Previous());
                   });
    server.AddTool("self.music.get_status",
                   "Get TF music state, song ID, progress and card status.", PropertyList(),
                   [](const PropertyList&) -> ReturnValue { return MakeStatus(); });
}
