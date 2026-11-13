# Streams of the assistant tests

Server-sent event streams of chat completions, written by hand in the shape of answers recorded from OpenRouter on
2026-11-13 (the order and the fields of the deltas, the keep-alive comment, the second finishing chunk with the usage,
`data: [DONE]`); the recordings themselves were cut at 300 characters a line and are not in the repository. The ids,
the reasoning signature and the texts are invented. Embedded into the tests by `quench_embed_test_data`
(`tests/CMakeLists.txt`).

- `parallel_tools.sse`: two parallel tool calls (`get_status` with `{}`, `list_units` with its arguments in two
  deltas), in the shape of `openai/gpt-6-luna`'s answer.
- `text_answer.sse`: a text answer in five deltas with a non-ASCII character, a `reasoning_details` delta and two
  keep-alive comments, in the shape of `anthropic/claude-haiku-5.5`'s answer.
