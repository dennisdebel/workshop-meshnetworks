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

#define CHUNK_SIZE 128

#define MAX_UPLOAD_SIZE 100000 //(100Kb)

// -----------------------
// Globals
// -----------------------

uint32_t myNodeId;

std::vector<uint32_t> meshNodes;

painlessMesh mesh;

AsyncWebServer server(80);

Scheduler userScheduler;

// -----------------------
// Webring proxy globals
// -----------------------

AsyncWebServerRequest *proxyRequest = nullptr;

AsyncResponseStream *proxyStream = nullptr;

bool proxyActive = false;

bool proxyReady = false;

uint32_t proxyNode = 0;

String proxyFile = "";

bool proxyTransfer = false;

String proxyData = "";

String lastProxyResult = "";

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

  struct MeshRequest
  {
      uint32_t node;
      String message;
  };
  
  std::vector<MeshRequest> meshRequestQueue;

  
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
    300, //slowwwwwww ..or fast..thats the question...
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

//    // -----------------------
//    // FILE LIST REQUEST
//    // -----------------------
//
//    if(msg == "FILE_LIST")
//    {
//
//        Dir dir = LittleFS.openDir("/");
//
//        while(dir.next())
//        {
//            String filename = dir.fileName();
//
//            if(isSyncIgnored(filename)) // ignore local files
//              continue;
//        
//            if(filename.endsWith(".tmp"))
//            {
//                continue;
//            }
//        
//            String reply;
//            reply.reserve(80);
//          
//            reply = "FILE:";
//            reply += filename;
//            reply += ":";
//            reply += dir.fileSize();
//        
//            mesh.sendSingle(
//                from,
//                reply
//            );
//        }
//
//        return;
//    }

//    // -----------------------
//    // FILE ANNOUNCEMENT
//    // -----------------------
//
//    if(msg.startsWith("FILE:"))
//    {
//    
//        int p1 = msg.indexOf(':',5);
//    
//        String filename =
//            msg.substring(5,p1);
//
//        if(isSyncIgnored(filename)) 
//        {
//            Serial.println("Ignoring LOCAL file");
//            return;
//        }
//    
//        size_t filesize =
//            msg.substring(p1+1).toInt();
//    
//        Serial.print("Remote file: ");
//        Serial.print(filename);
//        Serial.print(" ");
//        Serial.println(filesize);
//    
//        if(!hasFile(filename) && requestedFile != filename)
//        {
//            requestedFile = filename;
//            requestedFrom = from;
//    
//            String request;
//            request.reserve(80);
//    
//            request = "GET_FILE:";
//            request += filename;
//    
//            mesh.sendSingle(
//                from,
//                request
//            );
//    
//        }
//        else
//        {
//            Serial.println("Already requested");
//        }
//    
//        return;
//    }


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
    
    if(proxyTransfer)
    {
        Serial.println("Proxy receiving file");
    
        return;
    }
            
    int p1 =
            msg.indexOf(':',11);

        incoming.name =
            msg.substring(11,p1);

        incoming.size =
            msg.substring(p1+1).toInt();

        incoming.received = 0;
        
        receivingFile=true;

        String tempName;

        tempName.reserve(80);
        
        tempName = incoming.name;
        
        if(!tempName.endsWith(".tmp"))
        {
            tempName += ".tmp";
        }

        if(getFreeSpace() < incoming.size) // prevent full nodes to accept transfers!
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
    
        String encoded =
            msg.substring(p2+1);
    
    
        //uint8_t decoded[400];
        uint8_t decoded[400] = {0};
    
        int decodedLen =
            base64Decode(
                encoded,
                decoded
            );
    
    
        if(proxyTransfer)
        {
            Serial.println("Appending proxy data");
    
            for(int i = 0; i < decodedLen; i++)
            {
                //proxyData += (char)decoded[i]; //TODO old
                if(decoded[i] != 0)
                {
                    proxyData += (char)decoded[i];
                }
            }
    
            Serial.print("Proxy length now: ");
            Serial.println(proxyData.length());
    
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
      
      if(proxyTransfer)
      {
          // remove accidental null bytes at the end
          while(proxyData.length() > 0 &&
                proxyData[proxyData.length()-1] == '\0')
          {
              proxyData.remove(proxyData.length()-1);
          }
      
      
          // Rewrite resource paths BEFORE sending response
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
      
      
          Serial.println("Proxy file complete");
          Serial.println(proxyData.substring(0,200));
      
      
          if(proxyRequest != nullptr)
          {
              Serial.println("Sending delayed HTTP response");
      
              proxyRequest->send(
                  200,
                  proxyMime,
                  proxyData
              );
      
              proxyRequest = nullptr;
          }
      
      
          proxyReady = true;
          proxyTransfer = false;
      
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
      
  server.on(
    "/mesh/*",
    HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
  
      Serial.println("=== MESH PROXY REQUEST ===");
  
  
      String url = request->url();
  
      Serial.print("URL: ");
      Serial.println(url);
  
  
      // Example:
      // /mesh/2880712319/test.txt
  
  
      int firstSlash = url.indexOf('/', 6);
  
  
      if(firstSlash == -1)
      {
          request->send(
              400,
              "text/plain",
              "Bad mesh URL"
          );
          return;
      }
  
  
      String nodeString =
          url.substring(6, firstSlash);
  
  
      uint32_t node =
          strtoul(
              nodeString.c_str(),
              NULL,
              10
          );
  
  
      String path =
          url.substring(firstSlash);
      
      proxyMime = getMimeType(path);
      
      proxyNode = node;

          
      Serial.print("Node: ");
      Serial.println(node);
  
  
      Serial.print("Path: ");
      Serial.println(path);
  
      String msg;
  
      msg = "GET_FILE:";
      msg += path;
  
      Serial.print("Message: ");
      Serial.println(msg);
  
  
  
      MeshRequest req;
  
      req.node = node;
      req.message = msg;
  
  
      proxyTransfer = true;
      proxyReady = false;
      proxyData = "";
      
      meshRequestQueue.push_back(req);
  
  
      Serial.println("Queued mesh request");
  
  
      Serial.print("Queue size: ");
      Serial.println(meshRequestQueue.size());
  
      proxyActive = true;
      
      proxyReady = false;
      proxyData = "";

//      while(!proxyReady)
//      {
//          delay(10); //cases crashes
//      }

        proxyRequest = request;
        
//      request->send(
//          200,
//          proxyMime,
//          proxyData
//      );
  
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

//  static unsigned long syncTimer=0;
//
//  if(!sendingFile && !receivingFile)
//  {
//      if(millis()-syncTimer>30000)
//      {
//          syncTimer=millis();
//  
//          if(mesh.getNodeList().size()>0)
//          {
//              mesh.sendBroadcast("FILE_LIST"); 
//              Serial.println("Requesting file list");
//          }
//      }
//  }
}
