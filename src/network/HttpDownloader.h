#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

// Forward-declared: fetchUrl() takes a Stream& by reference. On-device this name
// arrives transitively via the SdFat/Arduino chain in <HalStorage.h>; declaring
// it here keeps the header self-sufficient (the host build's SdFat shim doesn't
// pull Arduino's Stream in transitively).
class Stream;

/**
 * HTTP client utility for fetching content and downloading files.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * POST a JSON body and stream the response through onData as it arrives.
   *
   * Written for streaming chat completions, where the response is an
   * open-ended Server-Sent Events stream rather than a sized body: onData
   * returning false tears the transfer down at the next chunk, which is how a
   * caller stops at the provider's terminal sentinel instead of waiting for
   * the server to close, and how a user cancels mid-generation.
   *
   * bearerToken, when non-empty, is sent as `Authorization: Bearer`. Redirects
   * are deliberately not followed: an API endpoint that answers a POST with a
   * 3xx is misconfigured, and replaying a credential to whatever host it names
   * is not a thing to do automatically.
   *
   * Returns true only for a 2xx whose body was delivered in full or stopped by
   * onData. `outStatus`, when given, always receives the HTTP status (0 if the
   * request never got that far) so a caller can tell 401 from a dropped
   * connection.
   */
  static bool postJson(const std::string& url, const std::string& body, const DataCallback& onData,
                       const std::string& bearerToken = "", int* outStatus = nullptr);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};
