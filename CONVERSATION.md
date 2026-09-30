# Optional conversation behavior

All new behavior is disabled by default. Enable only what the realm needs in
`mod_ollama_chat.conf`, then run `.ollama reload`.

```ini
OllamaChat.Conversation.Enable = 1
OllamaChat.Conversation.HoldSeconds = 120
OllamaChat.Conversation.MaxDistance = 25
OllamaChat.Conversation.HoldStill = 1
OllamaChat.BlacklistMastersOnly = 1
OllamaChat.Delivery.Split = 1
OllamaChat.Delivery.MaxMessages = 3
```

When a bot accepts a nearby human's say/yell message, it holds that person's
attention and routes their next nearby lines back to the same bot. Naming
another bot changes the addressee. One bot and one person form each engagement;
forming another engagement releases the previous one. A delayed answer cannot
reclaim an ended engagement or switch the current partner. Group companions keep
their normal movement and are never held.

With `HoldStill` on, the bot stops and faces the speaker. It resumes its AI when
either participant enters combat, dies, leaves, moves out of range, or the
conversation expires. Flights/transports also release the hold. Disabling
conversation mode or the entire chat module releases held bots. Unrelated
ambient chatter waits while the bot is engaged. `HoldSeconds` is bounded to
10–3600 and `MaxDistance` to 5–100 yards.

`BlacklistMastersOnly` applies configured command prefixes only to bots mastered
by the speaker. An unowned bot can answer ordinary sentences such as “who are
you?” instead of dropping them as commands. Addon-language traffic remains
ignored and the existing master-command parser remains in use.

Split delivery sends a long generated reply as several paced chat messages.
Each is at most `Delivery.MaxMessageBytes` (32–255, default 255); `MaxMessages`
caps the number of messages (default 3, 0 means unlimited). The general
`MaxReplyLength` still caps the whole response, so raise it explicitly if needed.
Sentence/word boundaries are preferred and UTF-8 bytes are kept together.
Delivery filters run before splitting. Later messages recheck the speaker and
destination before sending, and do not make additional model calls.

Optional human-reply model routing uses `OllamaChat.Reply.Model`, `.Url` and
`.NumPredict`. An empty model keeps the normal model. An empty URL keeps the
normal endpoint, and a zero token cap keeps the normal cap. Bot-to-bot chatter
and classifiers retain their existing routes. Routed models do not use the
normal model's reasoning-capability cache; their requests ask for thinking off.
Generation configuration is copied through the existing mutex-protected
snapshot after prompt escape decoding; worker typing/delivery settings use the
same snapshot. All player/movement access stays on the world thread. Settings
reloads preserve unsaved conversation turns and sentiment changes instead of reloading live state.

Two optional prompt controls are also available: `OllamaChat.TemplateEscapes`
decodes literal `\n` and `\t` in configured templates, and
`OllamaChat.Roleplay.VoicePreamble` prepends operator-provided framing to race/class
voice hints. Both leave the existing prompts unchanged when unset.

`.ollama status` reports conversation mode, engaged count, split delivery, and
the optional reply model. Dashboard controls require the separate Settings
panel change in `mod-dashboard`; this module does not expose an HTTP API.

Validation: the pure text tests compile with no AzerothCore dependencies:

```sh
g++ -std=c++17 -Wall -Wextra -Isrc apps/text-test/text_test.cpp src/mod-ollama-chat_text.cpp -o /tmp/text_test
/tmp/text_test
```

In-game checks before release: speak to a named bot and follow up without its
name; address another bot before the first reply lands; walk away; enter combat; log out; turn off HoldStill,
conversation mode and the chat module while engaged. Check group companions
still move and fight. Test a long reply, target logout during delivery, and
separate reply/utility endpoints. With every new option off, compare normal
chat behavior. The draft has not yet completed a full realm build or these
in-game checks on the current upstream base.
