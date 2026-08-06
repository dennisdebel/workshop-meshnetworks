#include "./DNSServer.h"
#include "painlessMesh.h"
#include <vector>
#include <Arduino.h>
#include <LittleFS.h>

#ifdef ESP8266
#include <ESPAsyncTCP.h>
#else
#include <AsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>

// -----------------------
// DNS hijacking
// -----------------------

DNSServer dnsServer;
const byte DNS_PORT = 53;

// -----------------------
// Definitions 
// -----------------------

#define MESH_PREFIX     "webring"
#define MESH_PASSWORD   "12345678"
#define MESH_PORT       5555

#define CHUNK_SIZE 300 //was 120 // must be divideble by 3 (base64 triplets)

#define MAX_UPLOAD_SIZE 100000 //(100Kb)

// -----------------------
// Globals
// -----------------------

uint32_t myNodeId;

std::vector<uint32_t> meshNodes;

painlessMesh mesh;

AsyncWebServer server(80);

Scheduler userScheduler;

struct MeshRequest
  {
      uint32_t node;
  
      String message;
  
      String mime;
  
      String path;
  
      AsyncWebServerRequest *request;
};
  
// -----------------------
// Webring proxy globals
// -----------------------

std::vector<MeshRequest> meshRequestQueue;

MeshRequest activeProxy;

AsyncResponseStream *proxyStream = nullptr;

bool proxyActive = false;

bool proxyReady = false;

bool proxyTransfer = false;

// text content
String proxyData = "";

// binary content
uint8_t *proxyBuffer = nullptr;

size_t proxyLength = 0;

size_t proxyExpected = 0;

String lastProxyResult = "";

AsyncWebServerRequest *proxyRequest = nullptr;

uint32_t proxyNode = 0;

String proxyFile = "";

String proxyMime = "text/plain";

// -----------------------
// Incoming file
// -----------------------

unsigned long lastSyncRequest = 0; // Sync cool down

bool receivingFile=false; // Transfer locks
String requestedFile="";  // Transfer locks
uint32_t requestedFrom=0; // Transfer locks

struct IncomingFile
{
    String name;
    size_t size;
    size_t received;
    File file;
};

IncomingFile incoming;

// -----------------------
// Incoming file queue
// -----------------------

  struct FileRequest
  {
      uint32_t node;
      String filename;
  };
  
  std::vector<FileRequest> sendQueue;

  bool alreadyQueued(uint32_t node, String filename)
  {
      for(auto &req : sendQueue)
      {
          if(req.node == node && req.filename == filename)
              return true;
      }
 
      return false;
  }

// -----------------------
// Add mimetypes
// -----------------------

String getMimeType(String filename)
{
    if(filename.endsWith(".html"))
        return "text/html";

    if(filename.endsWith(".css"))
        return "text/css";

    if(filename.endsWith(".js"))
        return "application/javascript";

    if(filename.endsWith(".png"))
        return "image/png";

    if(filename.endsWith(".jpg") ||
       filename.endsWith(".jpeg"))
        return "image/jpeg";

    if(filename.endsWith(".gif"))
        return "image/gif";

    if(filename.endsWith(".svg"))
        return "image/svg+xml";

    if(filename.endsWith(".txt"))
        return "text/plain";

    return "application/octet-stream";
}


// -----------------------
// Mesh Request Queue
// -----------------------


  void processMeshRequests()
  {
      if(meshRequestQueue.empty())
          return;
  
      MeshRequest req =
          meshRequestQueue.front();
  
      meshRequestQueue.erase(
          meshRequestQueue.begin()
      );
  
      Serial.print("Sending queued request to ");
      Serial.println(req.node);
  
      bool result =
          mesh.sendSingle(
              req.node,
              req.message
          );
  
      Serial.print("Result:");
      Serial.println(result);
  }


  struct ProxyRequest
  {
      uint32_t requestId; // Unique ID for each request
      AsyncWebServerRequest *request;
      uint32_t node;
      String path;
      String mime;
  };
  
  std::vector<ProxyRequest> proxyQueue;
  
  // Change proxyRequest management variables:
  uint32_t activeRequestId = 0;
  uint32_t globalRequestIdCounter = 1;
  bool activeRequestValid = false;

  
// -----------------------
// File transfer state
// -----------------------

  uint8_t sendBuffer[CHUNK_SIZE];
  
  File outgoingFile;
  
  uint32_t outgoingNode;
  
  String outgoingFilename;
  
  bool sendingFile = false;
  
  size_t outgoingPos = 0;


// -----------------------
// TaskScheduler file sender
// Must be global
// -----------------------

void sendFileChunk();

Task fileTask(
    20, //slowwwwwww ..or fast..thats the question... //maybe needs to be faster///
    TASK_FOREVER,
    []()
    {
        sendFileChunk();
    }
);


// -----------------------
// Helpers
// -----------------------

bool hasFile(String name)
{
    return LittleFS.exists(name);
}

size_t getFreeSpace()
{
    FSInfo fs_info;
    LittleFS.info(fs_info);

    return fs_info.totalBytes - fs_info.usedBytes;
}


// Helper for local files (physical node specific files, like configs, bg images)
bool isSyncIgnored(String name)
{
    if(name.startsWith("LOCAL_"))
        return true;

    if(name.endsWith(".tmp"))
        return true;

    return false;
}

// -----------------------
// Base64
// -----------------------

const char b64chars[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64Encode(
    uint8_t *data,
    size_t len,
    String &output
)
{
    output = "";

    int i = 0;

    while(i < len)
    {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;


        uint32_t triple =
            (octet_a << 16) |
            (octet_b << 8) |
            octet_c;
            
        output += b64chars[(triple >> 18) & 0x3F];
        output += b64chars[(triple >> 12) & 0x3F];

        if(i - 2 < len)
            output += b64chars[(triple >> 6) & 0x3F];
        else
            output += '=';

        if(i - 1 < len)
            output += b64chars[triple & 0x3F];
        else
            output += '=';
    }
}

int base64Decode(
    String input,
    uint8_t *output
)
{
    int val = 0;
    int valb = -8;
    int outLen = 0;

    for(int i = 0; i < input.length(); i++)
    {
        char c = input[i];

        if(c == '=')
            break;

        int idx = -1;

        for(int j = 0; j < 64; j++)
        {
            if(b64chars[j] == c)
            {
                idx = j;
                break;
            }
        }

        if(idx < 0)
            continue;

        val = (val << 6) + idx;
        valb += 6;

        if(valb >= 0)
        {
            output[outLen++] =
                (val >> valb) & 0xFF;

            valb -= 8;
        }
    }

    return outLen;
}

// -----------------------
// File Queue
// ----------------------- 
void startNextFile()
{
    if(sendingFile)
        return;

    if(sendQueue.empty())
        return;

    FileRequest req = sendQueue.front();

    sendQueue.erase(sendQueue.begin());

    outgoingFile = LittleFS.open(
        req.filename,
        "r"
    );

    if(!outgoingFile)
    {
        Serial.println("Queued file missing");
        return;
    }

    outgoingNode = req.node;
    outgoingFilename = req.filename;
    outgoingPos = 0;

    String start;
    
    start = "FILE_START:";
    start += outgoingFilename;
    start += ":";
    start += outgoingFile.size();
    start += ":";
    start += getMimeType(outgoingFilename);

    mesh.sendSingle(
        outgoingNode,
        start
    );

    sendingFile=true;

    Serial.print("Starting queued transfer: ");
    Serial.println(outgoingFilename);
}


// -----------------------
// More queuing 
// -----------------------

void startNextProxy()
{
    if(proxyTransfer)
        return;

    if(proxyQueue.empty())
        return;

    ProxyRequest p = proxyQueue.front();
    proxyQueue.erase(proxyQueue.begin());

    proxyRequest = p.request;
    activeRequestId = p.requestId;
    activeRequestValid = true; // Mark active request valid
    proxyNode = p.node;
    proxyMime = p.mime;
    proxyFile = p.path;

    proxyData = "";
    proxyTransfer = true;

    MeshRequest req;
    req.node = proxyNode;
    req.message = "GET_FILE:" + proxyFile;

    meshRequestQueue.push_back(req);

    Serial.printf("Started proxy (ID %u): %s\n", activeRequestId, proxyFile.c_str());
}

// -----------------------
// Send File Chunk
// -----------------------

void sendFileChunk()
{

    if(!sendingFile)
        return;

    // guard heap, check before attempting file transfers
    if(ESP.getFreeHeap() < 20000)
    {
        Serial.println("LOW HEAP - waiting");
        return;
    }

    Serial.print("Heap: ");
    Serial.println(ESP.getFreeHeap());
    Serial.print("Offset: ");
    Serial.println(outgoingPos);
    
    if(!outgoingFile.available())
    {

        outgoingFile.close();

        String endMsg;
        endMsg.reserve(64);

        endMsg = "FILE_END:";
        endMsg += outgoingFilename;

        mesh.sendSingle(
            outgoingNode,
            endMsg
        );

        outgoingFilename = "";

        sendingFile = false;
        
        Serial.println("file sent");
        
        startNextFile();
        
        return;
    }

    int len =
        outgoingFile.read(
            sendBuffer,
            sizeof(sendBuffer)
        );

    String encoded;
    encoded.reserve(180);

    base64Encode(
        sendBuffer,
        len,
        encoded
    );
  
    String chunk;
    chunk.reserve(430);
      
    chunk = "FILE_CHUNK:";
    chunk += outgoingFilename;
    chunk += ":";
    chunk += outgoingPos;
    chunk += ":";
    chunk += encoded;

    mesh.sendSingle(
        outgoingNode,
        chunk
    );

    outgoingPos += len;
}

// -----------------------
// Mesh callback
// -----------------------

void receivedCallback(uint32_t from, String &msg)
{
    Serial.print("RX ");
    Serial.print(from);
    Serial.print(": ");

      if(!msg.startsWith("FILE_CHUNK:"))
      {
          Serial.println(msg);
      }



    // -----------------------
    // GET FILE
    // -----------------------

    if(msg.startsWith("GET_FILE:"))
    {

        Serial.print("GET_FILE requested by ");
        Serial.println(from);
        Serial.println(msg);
        
        String filename = msg.substring(9);

        if(isSyncIgnored(filename))
        {
            Serial.println("Refusing LOCAL transfer");
            return;
        }
    
        if(sendingFile)
        {
            Serial.println("Queueing transfer");
        
            FileRequest req;
        
            req.node = from;
            req.filename = filename;
        
            if(!alreadyQueued(from, filename))
            {
                sendQueue.push_back({from, filename});
                Serial.println("Queued");
            }
            else
            {
                Serial.println("Already queued");
            }

            bool alreadyQueued=false;
            
            for(auto &q : sendQueue)
            {
                if(q.node == from &&
                   q.filename == filename)
                {
                    alreadyQueued=true;
                    break;
                }
            }
            
            if(!alreadyQueued)
            {
                FileRequest req;
                req.node = from;
                req.filename = filename;
            
                sendQueue.push_back(req);
            }
        
            return;
        }

        outgoingFile =
            LittleFS.open(
                filename,
                "r"
            );

        if(!outgoingFile)
        {
            Serial.println("missing file");
            return;
        }

        outgoingNode = from;

        outgoingFilename = filename;

        outgoingPos = 0;

        String start;
        start.reserve(100);

        start = "FILE_START:";
        start += filename;
        start += ":";
        start += outgoingFile.size();
        start += ":";
        start += getMimeType(filename);

        mesh.sendSingle(
            from,
            start
        );

        sendingFile = true;

        Serial.println("File transfer queued");

        return;
    }


    // -----------------------
    // FILE START
    // -----------------------
    
    if(msg.startsWith("FILE_START:"))
    {
        Serial.print("proxyTransfer=");
        Serial.println(proxyTransfer);
        Serial.print("Heap before image: "); // debug
        Serial.println(ESP.getFreeHeap()); // debug
            
        int p1 = msg.indexOf(':', 11);
        int p2 = msg.indexOf(':', p1 + 1);
    
    
        String filename =
            msg.substring(11, p1);
    
    
        int filesize =
            msg.substring(p1 + 1, p2).toInt();
    
    
        String mime =
            msg.substring(p2 + 1);
    
    
        Serial.println("FILE:");
        Serial.println(filename);
        Serial.println(filesize);
        Serial.println(mime);
    
    
        // -----------------------
        // PROXY TRANSFER
        // -----------------------
    
        if(proxyTransfer)
        {
            Serial.println("Proxy receiving file");
    
    
            proxyMime = mime;
    
            proxyExpected = filesize;
    
    
            if(proxyMime.startsWith("image") ||
               proxyMime.startsWith("application"))
            {
                Serial.println("Allocating binary buffer");
    
    
//                proxyBuffer =
//                    (uint8_t*)malloc(proxyExpected);
  
                if(proxyBuffer)
                {
                    free(proxyBuffer);
                    proxyBuffer = nullptr;
                }
                
                // Pad size up to nearest 4-byte alignment bound
                size_t alignedSize = (proxyExpected + 3) & ~3; 
                proxyBuffer = (uint8_t*)malloc(alignedSize);
                
                if(proxyBuffer) {
                    memset(proxyBuffer, 0, alignedSize);
                }


    
                if(proxyBuffer == nullptr)
                {
                    Serial.println("BUFFER ALLOCATION FAILED");
                    return;
                }
    
    
                proxyLength = 0;
            }
            else
            {
                // html/css/js
                proxyData = "";
            }
    
    
            return;
        }
    
    
    
        // -----------------------
        // NORMAL FILE RECEIVE
        // -----------------------
    
        incoming.name = filename;
    
        incoming.size = filesize;
    
        incoming.received = 0;
    
    
        receivingFile=true;
    
    
        String tempName;
    
        tempName.reserve(80);
    
        tempName = incoming.name;
    
    
        if(!tempName.endsWith(".tmp"))
        {
            tempName += ".tmp";
        }
    
    
        if(getFreeSpace() < incoming.size)
        {
            Serial.println("Not enough storage!");
            return;
        }
    
    
        incoming.file =
            LittleFS.open(
                tempName,
                "w"
            );
    
    
        Serial.println("Receiving:");
        Serial.println(incoming.name);
    
    
        return;
    }


    // -----------------------
    // FILE CHUNK
    // -----------------------

    if(msg.startsWith("FILE_CHUNK:"))
    {
        Serial.println("ENTER FILE_CHUNK");
    
        int p1 = msg.indexOf(':',11);
        int p2 = msg.indexOf(':',p1+1);
    
        int offset =
            msg.substring(p1+1,p2).toInt();
    
        uint8_t decoded[512]; //or 512
        
        int decodedLen =
            base64Decode(
                msg.substring(p2+1),
                decoded
            );
    
    
        if(proxyTransfer)
        {
            Serial.println("Appending proxy data");
        
        
            if(proxyMime.startsWith("image") ||
               proxyMime.startsWith("application"))
            {
                // binary data
            
                if(proxyLength + decodedLen > proxyExpected)
                {
                    Serial.println("!!! PROXY BUFFER OVERFLOW !!!");
                    Serial.print("Expected: ");
                    Serial.println(proxyExpected);
            
                    Serial.print("Current: ");
                    Serial.println(proxyLength);
            
                    Serial.print("Incoming: ");
                    Serial.println(decodedLen);
            
                    return;
                }
            
            
                memcpy(
                    proxyBuffer + proxyLength,
                    decoded,
                    decodedLen
                );
            
            
                proxyLength += decodedLen;
            
            
                Serial.print("Binary length now: ");
                Serial.println(proxyLength);
            }
            else
            {
                // text data (html/css/js)
        
                proxyData.reserve(proxyExpected);
        
                for(int i = 0; i < decodedLen; i++)
                {
                    proxyData += (char)decoded[i];
                    
                }
        
        
                Serial.print("Text length now: ");
                Serial.println(proxyData.length());
            }
        
        
            return;
        }
    
    
        if(incoming.file)
        {
            incoming.file.seek(offset);
    
            incoming.file.write(
                decoded,
                decodedLen
            );
    
            incoming.received += decodedLen;
        }
    
        return;
    }

    // -----------------------
    // FILE END
    // -----------------------

    if(msg.startsWith("FILE_END:"))
    {

      Serial.print("Heap after image: "); // debug
      Serial.println(ESP.getFreeHeap()); // debug

      if(proxyTransfer)
      {
          Serial.println("Proxy file complete");
      
      
          // -----------------------
          // TEXT FILES
          // -----------------------
      
          if(!proxyMime.startsWith("image") &&
             !proxyMime.startsWith("application"))
          {
      
              // remove accidental null bytes
              while(proxyData.length() > 0 &&
                    proxyData[proxyData.length()-1] == '\0')
              {
                  proxyData.remove(proxyData.length()-1);
              }
      
      
              // Rewrite paths in HTML
              if(proxyMime == "text/html")
              {
                  String prefix;
      
                  prefix = "/mesh/";
                  prefix += String(proxyNode);
                  prefix += "/";
      
      
                  proxyData.replace(
                      "href=\"",
                      String("href=\"") + prefix
                  );
      
                  proxyData.replace(
                      "src=\"",
                      String("src=\"") + prefix
                  );
              }
      
      
              Serial.println(proxyData.substring(0,200));
      
      
              if(proxyRequest != nullptr)
              {
                  Serial.println("Sending text response");
      
                  proxyRequest->send(
                      200,
                      proxyMime,
                      proxyData
                  );
      
                  proxyRequest = nullptr;
              }
      
          }
      
      
// -----------------------
          // BINARY FILES
          // -----------------------
          else
          {
              Serial.print("Binary size: ");
              Serial.println(proxyLength);

              // Check if browser connection is still valid!
              if (activeRequestValid && proxyRequest != nullptr)
              {
                  Serial.println("Sending binary response...");

                  uint8_t* rawBuf = proxyBuffer;
                  size_t totalLen = proxyLength;

                  proxyBuffer = nullptr; // Clear global pointer

                  AsyncWebServerResponse *response = proxyRequest->beginResponse(
                      proxyMime,
                      totalLen,
                      [rawBuf, totalLen](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                          size_t remaining = totalLen - index;
                          size_t copyLen = (remaining < maxLen) ? remaining : maxLen;

                          if (copyLen > 0 && rawBuf != nullptr) {
                              memcpy(buffer, rawBuf + index, copyLen);
                          }

                          if (index + copyLen >= totalLen) {
                              if (rawBuf != nullptr) {
                                  free(rawBuf);
                              }
                          }

                          return copyLen;
                      }
                  );

                  response->addHeader("Connection", "close");
                  proxyRequest->send(response);
              }
              else
              {
                  Serial.println("Client disconnected before download finished. Freeing buffer.");
                  if (proxyBuffer != nullptr) {
                      free(proxyBuffer);
                      proxyBuffer = nullptr;
                  }
              }

              proxyRequest = nullptr;
              activeRequestValid = false;
              proxyLength = 0;
          }
      
      
          proxyReady = true;
      
          proxyTransfer = false;
      
          proxyData = "";
      
      
          // continue queued browser requests
          proxyActive = false;

          proxyTransfer = false;
          
          
          proxyRequest = nullptr;
          
          
          if(proxyBuffer != nullptr)
          {
              free(proxyBuffer);
              proxyBuffer=nullptr;
          }
          
          
          startNextProxy();
      
      
          return;
      }

        if(incoming.file)
        {
            incoming.file.close();
    
            if(LittleFS.exists(incoming.name))
            {
                LittleFS.remove(incoming.name);
            }
    
            String tempName;
    
            tempName.reserve(80);
    
            tempName = incoming.name;
    
            if(!tempName.endsWith(".tmp"))
            {
                tempName += ".tmp";
            }
    
            LittleFS.rename(
                tempName,
                incoming.name
            );
        }
    
        Serial.println("Finished:");
        Serial.println(incoming.name);

        proxyActive=false;
    
        // release transfer lock
        receivingFile = false;
    
        // release request ownership
        requestedFile = "";
        requestedFrom = 0;
    
        return;
    }


//    // -----------------------
//    // NEW FILE
//    // -----------------------
//
//    if(msg.startsWith("NEW_FILE:"))
//    {
//
//        String filename =
//            msg.substring(9);
//
//        Serial.println("New file announced:");
//        Serial.println(filename);
// 
//        if(millis() - lastSyncRequest > 10000) // Sync cool down period
//        {
//            lastSyncRequest = millis();
//        
//            mesh.sendBroadcast(
//                "FILE_LIST"
//            );
//        }
//
//        return;
//    }

}

// -----------------------
// Upload handler
// -----------------------

void handleUpload(
  AsyncWebServerRequest *request,
  String filename,
  size_t index,
  uint8_t *data,
  size_t len,
  bool final
)
{

  if(index + len > MAX_UPLOAD_SIZE)
  {
      Serial.println("Upload too large");
      request->send(413, "text/plain", "File too large");
      return;
  }
  static File uploadFile;

  if(index == 0)
  {
    Serial.println("Upload start: " + filename);

    uploadFile = LittleFS.open(
      "/" + filename,
      "w"
    );
  }

  if(uploadFile)
  {
    uploadFile.write(data, len);
  }

  if(final)
  {
    uploadFile.close();

    Serial.println("Upload finished");
  }
}

// -----------------------
// Setup
// -----------------------

void setup()
{
  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("Starting...");

  // Filesystem
  if(!LittleFS.begin())
  {
    Serial.println("LittleFS failed!");
    return;
  }

  Serial.println("LittleFS OK");

  // Print files
  Dir dir = LittleFS.openDir("/");

  while(dir.next())
  {
    String name = dir.fileName(); // remove temp files
    if(name.endsWith(".tmp"))
    {
        Serial.print("Removing incomplete file: ");
        Serial.println(name);

        LittleFS.remove(name);
    }
    
    Serial.print("FILE: ");
    Serial.print(dir.fileName());
    Serial.print(" SIZE: ");
    Serial.println(dir.fileSize());
  }


  // Mesh
  mesh.setDebugMsgTypes(
    ERROR |
    STARTUP
  );

  mesh.init(
      MESH_PREFIX,
      MESH_PASSWORD,
      &userScheduler,
      MESH_PORT
  );

  myNodeId = mesh.getNodeId();
  
  Serial.print("My node ID: ");
  Serial.println(myNodeId);


  userScheduler.addTask(fileTask);
  fileTask.enable();
    
  Serial.println("Mesh started");
  
  Serial.print("AP SSID: ");
  Serial.println(MESH_PREFIX);
  
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  Serial.print("INTERNAL AP: ");
  Serial.println(WiFi.softAPSSID());

  mesh.onReceive(
    &receivedCallback
  );

  mesh.onNewConnection([](uint32_t nodeId){
      Serial.printf("NEW CONNECTION: %u\n", nodeId);
  });
  
  mesh.onChangedConnections([](){
  
      Serial.println("CONNECTIONS CHANGED");

      meshNodes.clear();

      SimpleList<uint32_t> nodes = mesh.getNodeList();
  
      Serial.print("Nodes: ");
  
      for(auto node : nodes)
      {
          Serial.print(node);
          Serial.print(" ");
          meshNodes.push_back(node);

      }
  
      Serial.println();
  });


  // -----------------------
  // Web root
  // -----------------------

  int boxWidth = 36;
  String border = "+----------------------------------+";


  server.on(
  "/proxy",
  HTTP_GET,
  [](AsyncWebServerRequest *request)
  {
      request->send(
          200,
          "text/plain",
          lastProxyResult
      );
  });

  
  

  
  // -----------------------
  // Mesh proxy routing
  // -----------------------
  server.on(
    "/proxy-status",
    HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
  
      if(proxyReady)
      {
          request->send(
              200,
              "application/json",
              "{\"status\":\"ready\"}"
          );
      }
      else
      {
          request->send(
              200,
              "application/json",
              "{\"status\":\"loading\"}"
          );
      }
  
    });


  server.on(
  "/proxy-data",
  HTTP_GET,
  [](AsyncWebServerRequest *request)
  {
    
      Serial.println(proxyMime);
      Serial.println(proxyData.substring(0,100));
      
      Serial.println("=== PROXY DATA REQUEST ===");
  
      AsyncResponseStream *response =
          request->beginResponseStream(proxyMime);
  
      response->addHeader(
          "Cache-Control",
          "no-cache, no-store, must-revalidate"
      );
  
      response->addHeader(
          "Pragma",
          "no-cache"
      );
  
      response->addHeader(
          "Expires",
          "0"
      );
  
      response->print(proxyData);
  
      request->send(response);
  
  });

  // retrieve own node idx
  server.on( 
    "/node-id",
    HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
        request->send(
            200,
            "text/plain",
            String(mesh.getNodeId())
        );
    }
  );

  // retrieve other node id's
  server.on(
    "/nodes",
    HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
  
        String json = "[";
  
        auto nodes = mesh.getNodeList();
  
        bool first = true;
  
        for(auto node : nodes)
        {
            if(!first)
                json += ",";
  
            json += String(node);
  
            first=false;
        }
  
        json += "]";
  
  
        Serial.println("NODE LIST:");
        Serial.println(json);
  
  
        request->send(
            200,
            "application/json",
            json
        );
  
    }
  );
  
  server.on(
      "/mesh/*",
      HTTP_GET,
      [](AsyncWebServerRequest *request)
      {

        request->addInterestingHeader("Keep-Alive"); // Set keep-alive on the initial request

        Serial.println("=== MESH PROXY REQUEST ===");

        String url = request->url();
        int firstSlash = url.indexOf('/', 6);
  
        if(firstSlash == -1)
        {
            request->send(400, "text/plain", "Bad mesh URL");
            return;
        }
  
        String nodeString = url.substring(6, firstSlash);
        uint32_t node = strtoul(nodeString.c_str(), NULL, 10);
        String path = url.substring(firstSlash);
  
        // Duplicate check
        for (const auto& existing : proxyQueue) {
                  if (existing.node == node && existing.path == path) {
                      // Only drop if client is still actively waiting
                      if (existing.request != nullptr && existing.request->client()->connected()) {
                          request->send(429, "text/plain", "Already fetching asset");
                          return;
                      }
                  }
              }
  
        if (proxyTransfer && proxyNode == node && proxyFile == path) {
            request->send(429, "text/plain", "Already fetching asset");
            return;
        }
  
        uint32_t reqId = globalRequestIdCounter++;
  
        // If browser disconnects mid-transfer, invalidate the request pointer safely
        request->onDisconnect([reqId]() {
            Serial.printf("Client disconnected for reqId: %u\n", reqId);
            if (activeRequestId == reqId) {
                activeRequestValid = false;
            }
            // Remove from queue if still pending
            for (auto it = proxyQueue.begin(); it != proxyQueue.end(); ) {
                if (it->requestId == reqId) {
                    it = proxyQueue.erase(it);
                } else {
                    ++it;
                }
            }
        });
  
        ProxyRequest p;
        p.requestId = reqId;
        p.request = request;
        p.node = node;
        p.path = path;
        p.mime = getMimeType(path);
        
        proxyQueue.push_back(p);
        
        Serial.print("Added HTTP proxy request ID: ");
        Serial.println(reqId);
      });

  // -----------------------
  // onnotfound....captive portal hack?
  // -----------------------
  
//  server.onNotFound(
//  [](AsyncWebServerRequest *request)
//  {
//
//
//      Serial.println("A");
//      String url = request->url();
//
//      
//      Serial.print("Not found: ");
//      Serial.println(url);
//         Serial.println("b");
//  
//      if(url.startsWith("/mesh/"))
//      {
//    
//          Serial.println("Mesh proxy request!");
//             Serial.println("C");
//  
//          int firstSlash = url.indexOf('/',6);
//  
//  
//          String nodeString =
//              url.substring(6, firstSlash);
//  
//  
//          uint32_t node =
//              strtoul(nodeString.c_str(), NULL, 10);
//  
//  
//          String path =
//              url.substring(firstSlash);
//  
//  
//          Serial.print("Node: ");
//          Serial.println(node);
//  
//          Serial.print("Path: ");
//          Serial.println(path);
//  
//  
//          proxyRequest = true;
//          proxyNode = node;
//          proxyFile = path;
//  
//  
////          String msg;
////  
////          msg = "GET_FILE:";
////          msg += path;
////
////
////          meshRequestQueue.push_back(
////          {
////              node,
////              msg
////          });
////          
////          Serial.println("Queued mesh request");
//
//          String msg;
//          
//          msg = "GET_FILE:";
//          msg += path;
//
//          
//Serial.println("BEFORE QUEUE");
//
//MeshRequest req;
//
//req.node = node;
//req.message = msg;
//
//Serial.println("AFTER CREATE");
//
//meshRequestQueue.push_back(req);
//
//Serial.println("AFTER QUEUE");
//
//Serial.println("Queued mesh request");
//          
//          Serial.print("Queue size: ");
//          Serial.println(meshRequestQueue.size());
//
//          Serial.println("Queued mesh request");
//  
//          request->send(
//              200,
//              "text/plain",
//              "proxy request sent"
//          );
//  
//  
//          return;
//      }
//  
//  
//      request->send(
//          404,
//          "text/plain",
//          "Not found"
//      );
//  
//  });

  // -----------------------
  // Download
  // -----------------------

  server.on("/download",
  HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    if(!request->hasParam("file"))
    {
      request->send(
        400,
        "text/plain",
        "missing file"
      );

      return;
    }

    String filename =
      request->getParam("file")->value();

    request->send(
      LittleFS,
      filename,
      "text/plain"
    );

  });



  // -----------------------
  // Upload
  // -----------------------

  server.on(
    "/upload",
    HTTP_POST,

    [](AsyncWebServerRequest *request)
    {
      request->redirect("/");
    },

    handleUpload
  );

  
  

          
  server.begin();
  
  server.serveStatic(
      "/",
      LittleFS,
      "/"
  )
  .setDefaultFile("index.html");

  Serial.println("HTTP server started");

//  // Set a custom hostname for the device, not sure what this does (nothing it seems)
//  bool success = mesh.setHostname("webring");
//    if (success) {
//      Serial.println("Hostname set successfully.");
//    } else {
//      Serial.println("Failed to set hostname.");
//    }
//  

  delay(1000); // maybe not needed

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP()); // redirect dns request to AP ip

} // end of setup()



// -----------------------
// Loop
// -----------------------

void loop()
{
  mesh.update();

  processMeshRequests(); // actually http requests

  dnsServer.processNextRequest();

  userScheduler.execute(); //send file chunks

  startNextProxy(); // process request queue

}
