// tinyhttpd Copyright (C) 2024 kernaltrap8
// This program comes with ABSOLUTELY NO WARRANTY
// This is free software, and you are welcome to redistribute it
// under certain conditions

/*
  tinyhttpd.cpp
*/

#include "tinyhttpd.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace tinyhttpd {
std::string basePath = ".";
volatile sig_atomic_t exitFlag = 0;
bool debugMode = false;
bool sslEnabled = false; // Flag to check if SSL is enabled
std::string sslCertPath; // Path to SSL certificate

const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string RESET = "\033[0m";

// Struct to hold data for argument parsing
struct Argument {
  std::string flag;
  std::string value;
};

// Function to parse argument data from Argument stuct
std::unordered_map<std::string, std::string> ParseArguments(int argc,
                                                            char *argv[]) {
  std::unordered_map<std::string, std::string> arguments;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg.size() > 1 && arg[0] == '-') {
      // Remove leading dash
      arg.erase(0, 1);

      // Check if there's an associated value
      std::string value;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        value = argv[++i];
      }

      // Store flag and value pair
      arguments[arg] = value;
    }
  }

  return arguments;
}

void PrintCurrentOperation(const std::string operation) {
  std::cout << "[" << GREEN << "SERVER" << RESET << "] " << operation
            << std::endl;
}

void LogRequest(const std::string &ipAddress, const std::string &requestTime,
                const std::string &method, const std::string &requestPath,
                const std::string &httpVersion, int statusCode,
                const std::string &request) {
  std::cout << ipAddress << " - - [" << requestTime << "] \"" << method << " "
            << requestPath << " " << httpVersion << "\" " << statusCode
            << std::endl;
  if (debugMode) {
    std::cerr << "[" << RED << "DEBUG" << RESET << "] Request: " << request
              << std::endl;
  }
}

void LogResponse(const std::string &response) {
  if (debugMode) {
    std::cerr << "[" << RED << "DEBUG" << RESET << "] Response: " << response
              << std::endl;
  }
}

std::string UrlDecode(const std::string &str) {
  std::string result;
  char decodeBuf[3] = {0};
  for (size_t i = 0; i < str.length(); ++i) {
    if (str[i] == '%') {
      if (i + 2 < str.length()) {
        decodeBuf[0] = str[i + 1];
        decodeBuf[1] = str[i + 2];
        result += static_cast<char>(strtol(decodeBuf, nullptr, 16));
        i += 2;
      }
    } else if (str[i] == '+') {
      result += ' ';
    } else {
      result += str[i];
    }
  }
  return result;
}

void ServeDirectoryListing(int ClientSocket, const std::string &directoryPath,
                           const std::string &requestPath, int portNumber) {
    std::stringstream response;    
    // Construct the response body
    response << "\r\n";
    response
        << "<html><head><title>Directory Listing</title></head>"
           "<style>"
           "html, body { height: 100%; margin: 0; }"
           "body { display: flex; flex-direction: column; margin: 0; }"
           "main { flex: 1; overflow-y: auto; padding: 10px; }"
           "ul { list-style-type: none; margin: 0; padding: 0; }"
           "li { padding-left: 20px; }"
           "li.directory::before { content: '\\1F4C1'; margin-right: 10px; }"
           "li.file::before { content: '\\1F4C4'; margin-right: 10px; }"
           "footer { background-color: #dddddd; padding: 7px; "
           "text-align: center; }"
           "</style>"
           "</head><body>\r\n"
           "<main>\r\n"
           "<h1 style=\"background-color: #dddddd; padding: 10px;\">Index of "
        << requestPath << "</h1>\r\n";
    response << "<ul>\r\n";

    // Add parent directory link
    if (requestPath != "/") {
        std::string parentPath = requestPath;
        if (parentPath.back() == '/') {
            parentPath.pop_back();
        }
        size_t pos = parentPath.find_last_of('/');
        if (pos != std::string::npos) {
            parentPath = parentPath.substr(0, pos);
        }
        if (parentPath.empty()) {
            parentPath = "/";
        }
        response << "<li class=\"directory\"><a href=\"" << parentPath
                 << "\">..</a></li>";
    }

    // Read directory contents and collect entries
    std::vector<std::string> directories;
    std::vector<std::string> files;
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(directoryPath.c_str())) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            std::string filename(ent->d_name);
            if (filename != "." && filename != "..") {
                std::string filePath = requestPath;
                if (requestPath.back() != '/') {
                    filePath += '/';
                }
                filePath += filename;

                struct stat pathStat;
                std::string fullPath = directoryPath + "/" + filename;
                stat(fullPath.c_str(), &pathStat);

                if (S_ISDIR(pathStat.st_mode)) {
                    directories.push_back(filename);
                } else {
                    files.push_back(filename);
                }
            }
        }
        closedir(dir);
    } else {
        response << "<p>Error reading directory.</p>\r\n";
    }

    // Sort directories and files alphabetically
    std::sort(directories.begin(), directories.end());
    std::sort(files.begin(), files.end());

    // Append directories
    for (const auto &dir : directories) {
        std::string filePath = requestPath + dir;
        response << "<li class=\"directory\"><a href=\"" << filePath << "\">" << dir
                 << "</a></li>\r\n";
    }

    // Append files
    for (const auto &file : files) {
        std::string filePath = requestPath + file;
        response << "<li class=\"file\"><a href=\"" << filePath << "\">" << file
                 << "</a></li>\r\n";
    }

    response << "</ul>\r\n";
    response << "</main>\r\n";
    response << "<footer>tinyhttpd/" << VERSION << " on " << GetLinuxDistribution()
             << " Serving port " << portNumber << "</footer>\r\n";
    response << "</body></html>\r\n";

    // Send the response
    std::string responseStr = response.str();
    std::string responseHeader = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(responseStr.length()) + "\r\n\r\n";
    send(ClientSocket, responseHeader.c_str(), responseHeader.length(), 0);
    send(ClientSocket, responseStr.c_str(), responseStr.length(), 0);
    close(ClientSocket);

    LogResponse(responseStr);
}

std::string GetLinuxDistribution() {
  std::ifstream file("/etc/os-release");
  std::string line;
  std::string distro;

  while (std::getline(file, line)) {
    if (line.find("PRETTY_NAME=") != std::string::npos) {
      // Extract everything after "PRETTY_NAME="
      distro = line.substr(line.find("=") + 1);
      // Remove quotes and any trailing newline character
      distro.erase(std::remove(distro.begin(), distro.end(), '\"'),
                   distro.end());
      break;
    }
  }

  return distro;
}

void HandleClientRequest(int ClientSocket, int portNumber) {
    char buffer[4096] = {0};
    int valread = read(ClientSocket, buffer, 4096);
    if (valread <= 0) {
        close(ClientSocket);
        return;
    }

    // Extract IP and Port
    sockaddr_storage clientAddr;
    socklen_t clientAddrLen = sizeof(clientAddr);
    getpeername(ClientSocket, (sockaddr *)&clientAddr, &clientAddrLen);

    char clientIP[INET6_ADDRSTRLEN];
    if (clientAddr.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&clientAddr;
        inet_ntop(AF_INET, &s->sin_addr, clientIP, sizeof(clientIP));
    } else if (clientAddr.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&clientAddr;
        inet_ntop(AF_INET6, &s->sin6_addr, clientIP, sizeof(clientIP));
    }

    std::string realClientIP = clientIP;

    // Parse HTTP Request
    std::string request(buffer);
    std::istringstream requestStream(request);
    std::string line;
    std::string xForwardedForIP;

    // Look for the X-Forwarded-For header
    while (std::getline(requestStream, line) && line != "\r") {
        if (line.find("X-Forwarded-For:") != std::string::npos) {
            size_t colonPos = line.find(":");
            if (colonPos != std::string::npos) {
                std::string forwardedFor = line.substr(colonPos + 1);
                forwardedFor.erase(0, forwardedFor.find_first_not_of(" \t")); // Trim leading spaces
                forwardedFor.erase(forwardedFor.find_last_not_of(" \t") + 1); // Trim trailing spaces
                xForwardedForIP = forwardedFor;
                break;
            }
        }
    }

    if (!xForwardedForIP.empty()) {
        realClientIP = xForwardedForIP;
    }

    size_t pos1 = request.find(" ");
    size_t pos2 = request.find(" ", pos1 + 1);
    size_t pos3 = request.find("\r\n");
    if (pos1 == std::string::npos || pos2 == std::string::npos || pos3 == std::string::npos) {
        close(ClientSocket);
        return;
    }

    std::string method = request.substr(0, pos1);
    std::string requestPath = request.substr(pos1 + 1, pos2 - pos1 - 1);
    std::string httpVersion = request.substr(pos2 + 1, pos3 - pos2 - 1);

    // Decode the request path
    requestPath = UrlDecode(requestPath);

    // Generate request time
    time_t now = time(0);
    tm *gmtm = gmtime(&now);
    char requestTime[64];
    strftime(requestTime, sizeof(requestTime), "%d/%b/%Y:%H:%M:%S %z", gmtm);

    // Determine HTTP status code
    int statusCode = 200; // Default to 200 OK

    // Log the request
    LogRequest(realClientIP, requestTime, method, requestPath, httpVersion, statusCode, request);

    // Handle serving files
    std::string filePath = basePath + requestPath;
    struct stat pathStat;
    stat(filePath.c_str(), &pathStat);

    if (S_ISDIR(pathStat.st_mode)) {
        // Check if there's an index.html file in the directory
        std::string indexPath = filePath + "/index.html";
        struct stat indexStat;
        if (stat(indexPath.c_str(), &indexStat) == 0 && S_ISREG(indexStat.st_mode)) {
            // Serve index.html
            filePath = indexPath;
        } else {
            // Serve directory listing
            if (requestPath.back() != '/') {
                requestPath += '/';
            }
            std::string directoryPath = basePath + requestPath;
            ServeDirectoryListing(ClientSocket, directoryPath, requestPath, portNumber);
            return;
        }
    }

    // Serve file content
    std::ifstream fileStream(filePath, std::ios::in | std::ios::binary);
    if (!fileStream.is_open()) {
        statusCode = 404; // Not Found
        fileStream.close();
        ServeDirectoryListing(ClientSocket, basePath + "/", "/", portNumber);
        return;
    }

    // Prepare response based on status code and content of file
    std::stringstream response;
    response << "HTTP/1.1 ";
    switch (statusCode) {
        case 100:
            response << "100 Continue\r\n";
            break;
        case 101:
            response << "101 Switching Protocols\r\n";
            break;
        case 200:
            response << "200 OK\r\n";
            break;
        case 404:
            response << "404 Not Found\r\n";
            break;
        default:
            response << "500 Internal Server Error\r\n";
    }

    // Determine Content-Type
    std::string contentType;
    bool isTextOrHtml = false;
    if (filePath.find(".html") != std::string::npos) {
        contentType = "text/html";
        isTextOrHtml = true;
    } else if (filePath.find(".txt") != std::string::npos) {
        contentType = "text/plain";
        isTextOrHtml = true;
    } else if (filePath.find(".jpg") != std::string::npos || filePath.find(".jpeg") != std::string::npos) {
        contentType = "image/jpeg";
    } else if (filePath.find(".png") != std::string::npos) {
        contentType = "image/png";
    } else {
        contentType = "application/octet-stream"; // Default
    }

    // Read file content into response
    std::stringstream fileContent;
    fileContent << fileStream.rdbuf();
    fileStream.close();

    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << fileContent.str().length() << "\r\n";
    if (!isTextOrHtml) {
        response << "Content-Disposition: attachment; filename=\"" << requestPath.substr(requestPath.find_last_of("/") + 1) << "\"\r\n";
    }
    response << "\r\n";
    response << fileContent.str();

    // Send response
    std::string responseStr = response.str();
    send(ClientSocket, responseStr.c_str(), responseStr.length(), 0);
    close(ClientSocket);

    LogResponse(responseStr);
}

int BindToClientSocket(int SocketToBind) {
  int ServerSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (ServerSocket == 0) {
    std::cerr << "Failed to initialize socket. Exiting.\n";
    return 1;
  }

  // Set socket option to reuse address
  int opt = 1;
  if (setsockopt(ServerSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
      0) {
    std::cerr << "setsockopt(SO_REUSEADDR) failed.\n";
    return 1;
  }

  sockaddr_in SocketAddress;
  SocketAddress.sin_family = AF_INET;
  SocketAddress.sin_addr.s_addr = INADDR_ANY;
  SocketAddress.sin_port = htons(SocketToBind);

  if (bind(ServerSocket, (sockaddr *)&SocketAddress, sizeof(SocketAddress)) <
      0) {
    std::cerr << "Failed to bind to socket " << SocketToBind << ".\n";
    return 1;
  }

  if (listen(ServerSocket, 3) < 0) {
    std::cerr << "Listener failed.\n";
    return 1;
  }

  std::vector<std::thread> HttpdServerThread_t;

  while (!exitFlag) {
    int ClientSocket = accept(ServerSocket, nullptr, nullptr);
    if (ClientSocket < 0) {
      std::cerr << "Client socket accept failed.\n";
      return 1;
    }

    if (sslEnabled) {
      // Handle SSL connection
      SSL_CTX *sslContext = SSL_CTX_new(SSLv23_server_method());
      SSL *ssl = SSL_new(sslContext);
      SSL_set_fd(ssl, ClientSocket);
      if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        close(ClientSocket);
        continue;
      }
      // Handle SSL connection using ssl object
      // Example: SSL_read(ssl, ...), SSL_write(ssl, ...)
      HttpdServerThread_t.emplace_back(HandleClientRequest, SSL_get_fd(ssl),
                                       SocketToBind);
    } else {
      // Handle non-SSL connection
      HttpdServerThread_t.emplace_back(HandleClientRequest, ClientSocket,
                                       SocketToBind);
    }
  }

  // Join all threads before exiting
  for (auto &t : HttpdServerThread_t) {
    t.join();
  }

  // Close server socket before exiting
  close(ServerSocket);

  return 0;
}

} // namespace tinyhttpd

// Signal handler function
void signalHandler(int signum) {
  std::cerr << signum;
  std::cout << "\n";
  std::string ExitingServerString = "Exiting.";
  tinyhttpd::PrintCurrentOperation(ExitingServerString);
  // Set exit flag to terminate server gracefully
  tinyhttpd::exitFlag = 1;
  exit(1);
}

int main(int argc, char *argv[]) {

  std::unordered_map<std::string, std::string> arguments =
      tinyhttpd::ParseArguments(argc, argv);
  if (arguments.count("h") > 0 || arguments.count("help") > 0) {
    std::cout << "tinyhttpd - A small HTTP server\n"
              << "Usage: tinyhttpd -port <port_number> [-ssl <ssl_cert_path>] "
                 "[-d]\n\n"
              << "Options:\n"
              << "  -port <port_number>   Specify the port number to bind on\n"
              << "  -ssl <ssl_cert_path>  Enable SSL/TLS support and specify "
                 "SSL certificate path\n"
              << "  -d, --debug           Enable debug mode\n"
              << "  -v, --version         Display version information\n"
              << "  -h, --help            Display this help message\n\n"
              << "  -path                 Path to serve files from. Defaults "
                 "to \".\"\n\n"
              << "Examples:\n"
              << "  tinyhttpd -port 8080\n"
              << "  tinyhttpd -port 8443 -ssl /path/to/ssl/certificate.pem\n"
              << "  tinyhttpd -port 8000 -d\n\n";
    exit(0);
  }

  if (argc < 2) {
    std::cerr << "Please specify port to bind on.\n";
    exit(1);
  }

  if (arguments.count("v") > 0 || arguments.count("version") > 0) {
    std::cout << "tinyhttpd v" << VERSION << std::endl;
    exit(0);
  }

  if (arguments.count("port") == 0) {
    std::cerr << "Please specify port to bind on using -port <port_number>\n";
    exit(1);
  }

  if (arguments.count("d") > 0 || arguments.count("debug") > 0) {
    tinyhttpd::debugMode = true;
  }

  if (arguments.count("path") > 0) {
    tinyhttpd::basePath = arguments["path"];
  }

  if (arguments.count("ssl") > 0) {
    tinyhttpd::PrintCurrentOperation("Enabling SSL support.");
    tinyhttpd::sslEnabled = true;
    tinyhttpd::sslCertPath = arguments["ssl"];
    SSL_load_error_strings();
    SSL_library_init();
  }

  int portNumber = std::atoi(arguments["port"].c_str());

  // Register signal SIGINT and signal handler
  signal(SIGINT, signalHandler);

  std::string StartingServerString =
      "Starting tinyhttpd server on port " + std::to_string(portNumber);
  tinyhttpd::PrintCurrentOperation(StartingServerString);

  // Start the server
  return tinyhttpd::BindToClientSocket(portNumber);
}
