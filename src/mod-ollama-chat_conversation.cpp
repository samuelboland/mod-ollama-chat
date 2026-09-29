#include "mod-ollama-chat_conversation.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_world.h"

#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <chrono>
#include <cmath>
#include <unordered_map>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Engagement
    {
        uint64_t          personGuid = 0;
        Clock::time_point until;
    };

    // Both directions, so either side is found without a scan. World thread
    // only, so unlocked.
    std::unordered_map<uint64_t, Engagement> g_byBot;
    std::unordered_map<uint64_t, uint64_t>   g_byPerson;   // person -> bot

    // Long enough that the AI never gets a tick between two of ours; short
    // enough that a released bot is back to itself within a second.
    constexpr uint32_t HOLD_AI_MS    = 1500;
    constexpr uint32_t UPDATE_EVERY  = 500;

    uint64_t Raw(Player* p) { return p->GetGUID().GetRawValue(); }

    Player* Find(uint64_t raw) { return ObjectAccessor::FindConnectedPlayer(ObjectGuid(raw)); }

    // A bot drawn into combat, or on a flight path or boat, is never held.
    bool MayHold(Player* bot)
    {
        return bot->IsAlive() && !bot->IsInCombat() && !bot->IsInFlight() && !bot->GetTransport();
    }

    void Face(Player* bot, Player* person)
    {
        if (bot->HasUnitState(UNIT_STATE_CASTING) || bot->HasInArc(float(M_PI) / 6.0f, person))
            return;
        bot->SetFacingToObject(person);
    }

    void HoldStill(Player* bot, Player* person)
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!botAI)
            return;

        botAI->SetNextCheckDelay(HOLD_AI_MS);

        if (bot->isMoving())
        {
            bot->StopMoving();
            bot->GetMotionMaster()->Clear();
            bot->GetMotionMaster()->MoveIdle();
        }

        Face(bot, person);
    }

    void Release(uint64_t botGuid, const char* why)
    {
        auto it = g_byBot.find(botGuid);
        if (it == g_byBot.end())
            return;

        auto p = g_byPerson.find(it->second.personGuid);
        if (p != g_byPerson.end() && p->second == botGuid)
            g_byPerson.erase(p);
        g_byBot.erase(it);

        // Hand the bot back at once rather than when the last hold runs out:
        // if it is letting go because a fight started, it has to act now.
        if (Player* bot = Find(botGuid))
        {
            if (g_ConversationHoldStill)
                if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
                    botAI->SetNextCheckDelay(0);

            if (g_DebugEnabled)
                LOG_INFO("module.ollamachat", "[Ollama Chat] {} is done talking ({}).", bot->GetName(), why);
        }
    }

    // Why a held conversation should end now, or nullptr to keep it.
    const char* EndReason(Player* bot, Player* person, const Engagement& e, Clock::time_point now)
    {
        if (!bot || !bot->IsInWorld())
            return "bot gone";
        if (!person || !person->IsInWorld())
            return "person gone";
        if (!bot->IsAlive())
            return "bot died";
        if (bot->IsInCombat())
            return "combat";
        if (!bot->IsWithinDistInMap(person, g_ConversationMaxDistance))
            return "out of range";
        if (bot->IsInSameGroupWith(person))
            return "grouped";
        if (now >= e.until)
            return "silence";
        return nullptr;
    }
}

void Conversation_Engage(Player* bot, Player* person)
{
    if (!g_ConversationEnable || !bot || !person || bot == person)
        return;
    if (!OllamaIsRealPlayer(person) || OllamaIsRealPlayer(bot))
        return;
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        return;
    if (!MayHold(bot) || bot->IsInSameGroupWith(person) ||
        !bot->IsWithinDistInMap(person, g_ConversationMaxDistance))
        return;

    const uint64_t botGuid    = Raw(bot);
    const uint64_t personGuid = Raw(person);

    // One partner each way: whoever this person was talking to before, and
    // whoever this bot was talking to before, are let go.
    auto previous = g_byPerson.find(personGuid);
    if (previous != g_byPerson.end() && previous->second != botGuid)
        Release(previous->second, "they turned to someone else");

    auto current = g_byBot.find(botGuid);
    if (current != g_byBot.end() && current->second.personGuid != personGuid)
        Release(botGuid, "someone else spoke to it");

    const bool fresh = g_byBot.find(botGuid) == g_byBot.end();

    Engagement& e = g_byBot[botGuid];
    e.personGuid = personGuid;
    e.until      = Clock::now() + std::chrono::seconds(g_ConversationHoldSeconds);
    g_byPerson[personGuid] = botGuid;

    if (g_ConversationHoldStill)
        HoldStill(bot, person);

    if (fresh && g_DebugEnabled)
        LOG_INFO("module.ollamachat", "[Ollama Chat] {} stops to talk with {}.", bot->GetName(), person->GetName());
}

bool Conversation_IsEngaged(Player* bot, Player* person)
{
    if (!g_ConversationEnable || !bot || !person)
        return false;
    auto it = g_byBot.find(Raw(bot));
    return it != g_byBot.end() && it->second.personGuid == Raw(person);
}

Player* Conversation_PartnerAmong(Player* person, const std::vector<Player*>& candidates)
{
    if (!g_ConversationEnable || !person)
        return nullptr;
    auto it = g_byPerson.find(Raw(person));
    if (it == g_byPerson.end())
        return nullptr;
    for (Player* bot : candidates)
        if (bot && Raw(bot) == it->second)
            return bot;
    return nullptr;
}

void Conversation_Update(uint32_t diff)
{
    static uint32_t timer = 0;
    timer += diff;
    if (timer < UPDATE_EVERY)
        return;
    timer = 0;

    if (g_byBot.empty())
        return;

    // Switched off with a reload: let everyone go.
    if (!g_ConversationEnable)
    {
        std::vector<uint64_t> all;
        for (const auto& [botGuid, e] : g_byBot)
            all.push_back(botGuid);
        for (uint64_t botGuid : all)
            Release(botGuid, "conversation mode off");
        return;
    }

    const Clock::time_point now = Clock::now();
    std::vector<std::pair<uint64_t, const char*>> ending;

    for (const auto& [botGuid, e] : g_byBot)
    {
        Player* bot    = Find(botGuid);
        Player* person = Find(e.personGuid);
        if (const char* why = EndReason(bot, person, e, now))
        {
            ending.emplace_back(botGuid, why);
            continue;
        }
        if (g_ConversationHoldStill && MayHold(bot))
            HoldStill(bot, person);
    }

    for (const auto& [botGuid, why] : ending)
        Release(botGuid, why);
}

uint32_t Conversation_Count()
{
    return static_cast<uint32_t>(g_byBot.size());
}
