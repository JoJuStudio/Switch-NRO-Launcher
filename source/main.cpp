#include <switch.h>
#include <curl/curl.h>
#include <jansson.h>

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "token.h"

// -------------------- Model Types --------------------

struct Asset {
  std::string name;
  std::string url;
};

struct Release {
  std::string tag;
  std::string name;
  std::string createdAt;
  std::string commitId;
  std::string description;
  std::vector<Asset> assets;
};

// -------------------- CURL RAII Helpers --------------------

class CurlGlobal {
public:
  CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

class CurlEasy {
public:
  CurlEasy() : handle(curl_easy_init()) {
    if (!handle) {
      std::cerr << "CURL init failed\n";
      std::exit(1);
    }
  }

  ~CurlEasy() {
    if (handle)
      curl_easy_cleanup(handle);
  }

  void setopt(CURLoption opt, const char* v) {
    curl_easy_setopt(handle, opt, v);
  }

  void setopt(CURLoption opt, struct curl_slist* v) {
    curl_easy_setopt(handle, opt, v);
  }

  void setopt(CURLoption opt, long v) {
    curl_easy_setopt(handle, opt, v);
  }

  void performOrExit() {
    CURLcode res = curl_easy_perform(handle);
    if (res != CURLE_OK) {
      std::cerr << "CURL error: " << curl_easy_strerror(res) << "\n";
      std::exit(1);
    }
  }

  long getResponseCode() const {
    long code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code);
    return code;
  }

  CURL* getHandle() const { return handle; }

private:
  CURL* handle = nullptr;
};

// -------------------- Memory Buffer for CURL --------------------

class MemoryBuffer {
public:
  static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<MemoryBuffer*>(userdata);
    self->data.append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
  }

  std::string data;
};

// -------------------- JSON Helpers --------------------

static std::string jsonGetString(json_t* obj, const char* key) {
  if (!obj)
    return {};
  json_t* val = json_object_get(obj, key);
  return json_is_string(val) ? json_string_value(val) : "";
}

static std::string urlEncodeProject(const std::string& path) {
  std::string out;
  for (char c : path) {
    if (c == '/')
      out += "%2F";
    else
      out += c;
  }
  return out;
}

// -------------------- Parse Releases from JSON --------------------

static std::vector<Release> parseReleases(const std::string& rawJson) {
  std::vector<Release> result;
  json_error_t err;
  json_t* root = json_loads(rawJson.c_str(), 0, &err);
  if (!root) {
    std::cerr << "JSON parse error: " << err.text << "\n";
    return result;
  }

  if (!json_is_array(root)) {
    std::cerr << "Expected JSON array\n";
    json_decref(root);
    return result;
  }

  size_t idx;
  json_t* item;
  json_array_foreach(root, idx, item) {
    Release r;
    r.tag       = jsonGetString(item, "tag_name");
    r.name      = jsonGetString(item, "name");
    r.createdAt = jsonGetString(item, "created_at");

    json_t* commitObj = json_object_get(item, "commit");
    if (json_is_object(commitObj)) {
      r.commitId = jsonGetString(commitObj, "short_id");
    }

    json_t* assetsObj = json_object_get(item, "assets");
    if (json_is_object(assetsObj)) {
      json_t* links = json_object_get(assetsObj, "links");
      if (json_is_array(links)) {
        size_t ai;
        json_t* linkItem;
        json_array_foreach(links, ai, linkItem) {
          Asset a;
          a.name = jsonGetString(linkItem, "name");
          a.url = jsonGetString(linkItem, "direct_asset_url");
          if (a.url.empty())
            a.url = jsonGetString(linkItem, "url");
          if (!a.name.empty() && !a.url.empty())
            r.assets.push_back(a);
        }
      }

      json_t* sources = json_object_get(assetsObj, "sources");
      if (json_is_array(sources)) {
        size_t si;
        json_t* srcItem;
        json_array_foreach(sources, si, srcItem) {
          Asset a;
          std::string fmt = jsonGetString(srcItem, "format");
          a.name = "Source (" + fmt + ")";
          a.url = jsonGetString(srcItem, "url");
          if (!a.url.empty())
            r.assets.push_back(a);
        }
      }
    }

    result.push_back(std::move(r));
  }

  json_decref(root);
  return result;
}

// -------------------- Fetch Releases Over Network --------------------

static std::vector<Release> fetchReleases(const std::string& apiUrl,
                                          const std::string& token) {
  MemoryBuffer buf;
  CurlEasy curl;
  struct curl_slist* headers = nullptr;

  if (!token.empty()) {
    headers = curl_slist_append(headers, ("PRIVATE-TOKEN: " + token).c_str());
  }
  headers = curl_slist_append(headers, "Accept: application/json");

  curl.setopt(CURLOPT_HTTPHEADER, headers);
  curl.setopt(CURLOPT_URL, apiUrl.c_str());
  curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEFUNCTION,
                   MemoryBuffer::WriteCallback);
  curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEDATA, &buf);
  curl.setopt(CURLOPT_FOLLOWLOCATION, 1L);
  curl.performOrExit();

  long code = curl.getResponseCode();
  if (headers)
    curl_slist_free_all(headers);

  if (code != 200) {
    std::cerr << "HTTP error: " << code << "\n";
    return {};
  }

  return parseReleases(buf.data);
}

// -------------------- UI Helpers --------------------

static int runMenu(const std::vector<std::string>& items,
                   const std::string& title) {
  int sel = 0;
  PadState pad;

  padInitializeDefault(&pad);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);

  consoleClear();
  std::cout << title << "\n\n";
  for (int i = 0; i < (int)items.size(); ++i) {
    std::cout << (i == sel ? "> " : "  ") << items[i] << "\n";
  }
  consoleUpdate(nullptr);

  while (appletMainLoop()) {
    padUpdate(&pad);
    u64 btn = padGetButtonsDown(&pad);
    int prev_sel = sel;

    if (btn & HidNpadButton_Down)
      sel = (sel + 1) % items.size();
    if (btn & HidNpadButton_Up)
      sel = (sel - 1 + items.size()) % items.size();
    if (btn & HidNpadButton_A)
      return sel;
    if (btn & HidNpadButton_B)
      return -1;

    if (sel != prev_sel) {
      consoleClear();
      std::cout << title << "\n\n";
      for (int i = 0; i < (int)items.size(); ++i) {
        std::cout << (i == sel ? "> " : "  ") << items[i] << "\n";
      }
      consoleUpdate(nullptr);
    }

    svcSleepThread(50'000'000ULL);
  }

  return -1;
}

static void displayRelease(const Release& r, int idx, int total) {
  consoleClear();
  std::cout << "Release " << (idx + 1) << " of " << total << "\n\n"
            << "Tag:    " << r.tag << "\n"
            << "Name:   " << r.name << "\n"
            << "Commit: " << r.commitId << "\n"
            << "Date:   " << r.createdAt << "\n\n"
            << r.description << "\n\n";

  if (!r.assets.empty()) {
    std::cout << "Press X for assets, [+] to exit.\n";
  } else {
    std::cout << "No assets available for this release.\n"
              << "Press [+] to continue.\n";
  }
  consoleUpdate(nullptr);
}

// -------------------- Download Asset --------------------

struct DownloadCallbackData {
  std::atomic<bool>* canceled_ptr;
  std::atomic<curl_off_t>* dl_total_ptr;
  std::atomic<curl_off_t>* dl_now_ptr;
};

static int curlXferInfoCallback(void* clientp, curl_off_t dltotal,
                                curl_off_t dlnow, curl_off_t, curl_off_t) {
  auto* cb = static_cast<DownloadCallbackData*>(clientp);
  cb->dl_total_ptr->store(dltotal);
  cb->dl_now_ptr->store(dlnow);
  return cb->canceled_ptr->load() ? 1 : 0;
}

static void ensure_downloads_directory() {
  if (mkdir("sdmc:/downloads", 0777) != 0 && errno != EEXIST) {
    std::cerr << "Error creating downloads directory: " << errno << "\n";
  }
}

static void downloadAsset(const Asset& a, const std::string& token) {
  std::atomic<bool> done{false};
  std::atomic<bool> canceled{false};
  std::atomic<curl_off_t> dl_total{0};
  std::atomic<curl_off_t> dl_now{0};
  DownloadCallbackData cb{&canceled, &dl_total, &dl_now};

  // Prepare filename
  std::string filename = a.url.substr(a.url.find_last_of('/') + 1);
  if (filename.empty())
    filename = a.name;
  size_t qm = filename.find('?');
  if (qm != std::string::npos)
    filename.resize(qm);
  for (char& c : filename) {
    if (std::string("\\/:*?\"<>|").find(c) != std::string::npos)
      c = '_';
  }

  std::thread td([&]() {
    CurlEasy curl;
    struct curl_slist* headers = nullptr;
    if (!token.empty()) {
      headers = curl_slist_append(headers,
                                  ("PRIVATE-TOKEN: " + token).c_str());
      curl.setopt(CURLOPT_HTTPHEADER, headers);
    }

    std::string url = a.url;
    size_t jobs = url.find("/-/jobs/");
    if (jobs != std::string::npos) {
      size_t scheme_end = url.find("//") + 2;
      size_t domain_end = url.find('/', scheme_end);
      if (domain_end != std::string::npos && domain_end < jobs) {
        std::string domain = url.substr(0, domain_end);
        std::string project = url.substr(domain_end + 1, jobs - domain_end - 1);
        size_t id_start = jobs + 7; // len("/-/jobs/") == 7
        size_t id_end = url.find('/', id_start);
        std::string jobId = url.substr(id_start, id_end - id_start);
        size_t raw = url.find("/artifacts/raw/", id_end);
        if (raw != std::string::npos) {
          std::string rest = url.substr(raw + strlen("/artifacts/raw/"));
          url = domain + "/api/v4/projects/" + urlEncodeProject(project) + "/jobs/" + jobId + "/artifacts/" + rest;
        }
      }
    }

    curl.setopt(CURLOPT_URL, url.c_str());
    curl.setopt(CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.getHandle(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.getHandle(), CURLOPT_XFERINFOFUNCTION,
                     curlXferInfoCallback);
    curl_easy_setopt(curl.getHandle(), CURLOPT_XFERINFODATA, &cb);

    ensure_downloads_directory();
    std::string out = "sdmc:/downloads/" + filename;
    FILE* fp = fopen(out.c_str(), "wb");
    if (!fp) {
      std::cerr << "Failed to open " << out << "\n";
      done = true;
      if (headers)
        curl_slist_free_all(headers);
      return;
    }

    curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl.getHandle(), CURLOPT_WRITEDATA, fp);
    CURLcode res = curl_easy_perform(curl.getHandle());
    fclose(fp);

    if (res != CURLE_OK || canceled)
      remove(out.c_str());

    done = true;
    if (headers)
      curl_slist_free_all(headers);
  });

  PadState pad;
  padInitializeDefault(&pad);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);

  consoleClear();
  std::cout << "Downloading: " << a.name << "\nPress B to cancel.\n\n";
  consoleUpdate(nullptr);

  while (appletMainLoop() && !done) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_B)
      canceled = true;

    double progress =
        dl_total > 0 ? static_cast<double>(dl_now) / dl_total : 0.0;
    printf("\x1b[4;0H[");
    int w = 50;
    for (int i = 0; i < w; ++i)
      printf(i < progress * w ? "=" : " ");
    printf("] %.2f%%\n", progress * 100.0);

    consoleUpdate(nullptr);
    svcSleepThread(50'000'000ULL);
  }

  td.join();
  consoleClear();

  if (canceled) {
    std::cout << "Download cancelled.\n";
  } else if (dl_total > 0 && dl_now == dl_total) {
    std::cout << "Successfully downloaded: " << a.name << "\n";
  } else {
    std::cout << "Download failed: " << a.name << "\n";
  }

  std::cout << "\nPress A to continue.\n";
  consoleUpdate(nullptr);

  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_A)
      break;
    svcSleepThread(50'000'000ULL);
  }
}

// -------------------- Main --------------------

int main() {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  CurlGlobal curlInit;

  if (R_FAILED(socketInitializeDefault())) {
    std::cerr << "Socket init failed\n";
    consoleExit(nullptr);
    return 1;
  }

  nifmInitialize(NifmServiceType_User);
  nxlinkStdio();

  const std::string apiUrl =
      "https://gitlab.your-ass-is.exposed/api/v4/projects/"
      "craftcore%2Fclient-engine/releases";
  const char* envToken = std::getenv("GITLAB_PRIVATE_TOKEN");
  const std::string token = envToken && *envToken ? envToken : GITLAB_PRIVATE_TOKEN;

  if (token.empty() || token == "YOUR_ACTUAL_GITLAB_TOKEN_HERE") {
    consoleClear();
    std::cerr << "Error: Missing GitLab token\nPress [+] to exit.\n";
    consoleUpdate(nullptr);

    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        break;
      svcSleepThread(50'000'000ULL);
    }

    nifmExit();
    socketExit();
    consoleExit(nullptr);
    return 1;
  }

  std::cout << "Fetching releases...\n";
  consoleUpdate(nullptr);

  auto releases = fetchReleases(apiUrl, token);
  if (releases.empty()) {
    consoleClear();
    std::cout << "No releases found.\nPress [+] to exit.\n";
    consoleUpdate(nullptr);

    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        break;
      svcSleepThread(50'000'000ULL);
    }

    nifmExit();
    socketExit();
    consoleExit(nullptr);
    return 0;
  }

  int current = 0;
  displayRelease(releases[current], current, releases.size());

  PadState pad;
  padInitializeDefault(&pad);

  while (appletMainLoop()) {
    padUpdate(&pad);
    u64 btn = padGetButtonsDown(&pad);

    if (btn & HidNpadButton_Plus)
      break;

    if ((btn & HidNpadButton_X) && !releases[current].assets.empty()) {
      std::vector<std::string> names;
      for (auto& a : releases[current].assets)
        names.push_back(a.name);
      names.push_back("Back");

      int choice = runMenu(names, "Select asset:");
      if (choice >= 0 && choice < (int)names.size() - 1) {
        downloadAsset(releases[current].assets[choice], token);
      }
      displayRelease(releases[current], current, releases.size());
    }

    if (btn & (HidNpadButton_Down | HidNpadButton_Right)) {
      current = (current + 1) % releases.size();
      displayRelease(releases[current], current, releases.size());
    }

    if (btn & (HidNpadButton_Up | HidNpadButton_Left)) {
      current = (current - 1 + releases.size()) % releases.size();
      displayRelease(releases[current], current, releases.size());
    }

    consoleUpdate(nullptr);
    svcSleepThread(50'000'000ULL);
  }

  nifmExit();
  socketExit();
  consoleExit(nullptr);
  return 0;
}
