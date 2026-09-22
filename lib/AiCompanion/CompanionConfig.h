#pragma once

#include <cstddef>

// Connection settings for the companion, parsed from /companion/config.txt.
//
// Kept as an editable text file for the same reason the persona is: the
// endpoint, the model and the credential are configuration, not code, and a
// reader changing providers should not need a rebuild. It also keeps the
// four-button device out of the business of text entry for a URL and a key.
//
// Format is `key = value`, one per line, with '#' comments, blank lines, a
// UTF-8 BOM and CRLF all tolerated — the same forgiveness the persona parser
// gives, for the same reason.
//
// Recognised keys:
//   endpoint     full URL of an OpenAI-compatible chat completions endpoint
//   model        model name passed through in the request
//   key          bearer credential, sent as Authorization
//   max_tokens   optional cap on reply length; 0 leaves it to the persona
class CompanionConfig {
 public:
  static constexpr size_t ENDPOINT_BYTES = 256;
  static constexpr size_t MODEL_BYTES = 96;
  static constexpr size_t KEY_BYTES = 256;

  void clear();

  // Always leaves the object in a usable state. Returns true only when the
  // result is actually usable — an endpoint is present and is https or http.
  bool load(const char* data, size_t len);

  const char* endpoint() const { return endpointBuf; }
  const char* model() const { return modelBuf; }
  const char* key() const { return keyBuf; }
  int maxTokens() const { return maxTokensValue; }

  bool configured() const { return endpointBuf[0] != '\0'; }
  // True when the endpoint is plain http. Sending a credential over it is the
  // caller's decision to warn about, not this parser's to refuse.
  bool insecureEndpoint() const { return insecure; }
  // Keys the file contained that this build does not recognise; a typo in a
  // key name is otherwise completely silent.
  size_t unknownKeys() const { return unknown; }

 private:
  void assign(const char* name, size_t nameLen, const char* value, size_t valueLen);

  char endpointBuf[ENDPOINT_BYTES + 1]{};
  char modelBuf[MODEL_BYTES + 1]{};
  char keyBuf[KEY_BYTES + 1]{};
  int maxTokensValue = 0;
  bool insecure = false;
  size_t unknown = 0;
};
