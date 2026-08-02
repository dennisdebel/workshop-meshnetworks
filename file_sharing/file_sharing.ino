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
// painlessMesh settings
// -----------------------

#define MESH_PREFIX     "mesh"
#define MESH_PASSWORD   "12345678"
#define MESH_PORT       5555

#define CHUNK_SIZE 128

#define MAX_UPLOAD_SIZE 100000 //(100Kb)


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
// Globals
// -----------------------

painlessMesh mesh;

AsyncWebServer server(80);

Scheduler userScheduler;

 
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
    
//    if(msg.length()>50) // clogs up serial monitor
//    {
//        Serial.println(msg.substring(0,50));
//    }
//    else
//    {
//        Serial.println(msg);
//    }

      if(!msg.startsWith("FILE_CHUNK:"))
      {
          Serial.println(msg);
      }

    // -----------------------
    // FILE LIST REQUEST
    // -----------------------

    if(msg == "FILE_LIST")
    {

        Dir dir = LittleFS.openDir("/");


        while(dir.next())
        {
        
            String filename = dir.fileName();
        
            if(filename.endsWith(".tmp"))
            {
                continue;
            }
        
        
            String reply;
            reply.reserve(80);
        
        
            reply = "FILE:";
            reply += filename;
            reply += ":";
            reply += dir.fileSize();
        
        
            mesh.sendSingle(
                from,
                reply
            );
        }


        return;
    }





    // -----------------------
    // FILE ANNOUNCEMENT
    // -----------------------

if(msg.startsWith("FILE:"))
{

    int p1 = msg.indexOf(':',5);

    String filename =
        msg.substring(5,p1);

    size_t filesize =
        msg.substring(p1+1).toInt();


    Serial.print("Remote file: ");
    Serial.print(filename);
    Serial.print(" ");
    Serial.println(filesize);



    if(!hasFile(filename) && requestedFile != filename)
    {

        requestedFile = filename;
        requestedFrom = from;

        String request;
        request.reserve(80);

        request = "GET_FILE:";
        request += filename;

        mesh.sendSingle(
            from,
            request
        );

    }
    else
    {
        Serial.println("Already requested");
    }


    return;
}






    // -----------------------
    // GET FILE
    // -----------------------

    if(msg.startsWith("GET_FILE:"))
    {

        String filename =
            msg.substring(9);
            
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

        int p1 =
            msg.indexOf(':',11);



        incoming.name =
            msg.substring(11,p1);



        incoming.size =
            msg.substring(p1+1).toInt();


        incoming.received = 0;
        
//        if(incoming.name != requestedFile)
//        {
//            Serial.println("Unexpected file");
//            return;
//        }
        
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


        int p1 =
            msg.indexOf(':',11);


        int p2 =
            msg.indexOf(':',p1+1);



        int offset =
            msg.substring(
                p1+1,
                p2
            ).toInt();



        String encoded =
            msg.substring(p2+1);



        uint8_t decoded[400];



        int decodedLen =
            base64Decode(
                encoded,
                decoded
            );



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


    // release transfer lock
    receivingFile = false;

    // release request ownership
    requestedFile = "";
    requestedFrom = 0;


    return;
}

//    if(msg.startsWith("FILE_END:"))
//    {
//
//        if(incoming.file)
//        {
//            incoming.file.close();
//
//            if(LittleFS.exists(incoming.name))
//            {
//                LittleFS.remove(incoming.name);
//            }
//
//            String tempName;
//
//            tempName.reserve(80);
//
//            tempName = incoming.name;
//            
//            if(!tempName.endsWith(".tmp"))
//            {
//                tempName += ".tmp";
//            }
//
//            LittleFS.rename(
//                tempName,
//                incoming.name
//            );
//        }
//
//        Serial.println("Finished:");
//        Serial.println(incoming.name);
//
//        receivingFile=false; // transfer locks
//        requestedFile="";
//        requestedFrom=0;
//        String announce;
//
//        announce.reserve(80);
//
////        if(!incoming.name.endsWith(".tmp"))
////        {
////            announce = "NEW_FILE:";
////            announce += incoming.name;
////        
////            mesh.sendBroadcast(
////                announce
////            );
////        }
//
//          Serial.println("Finished:");
//          Serial.println(incoming.name);
//          
//          receivingFile=false;
//          
//          return;
//
////        mesh.sendBroadcast(
////            announce
////        );
//
//
//        //return;
//       
//    }




    // -----------------------
    // NEW FILE
    // -----------------------

    if(msg.startsWith("NEW_FILE:"))
    {

        String filename =
            msg.substring(9);



        Serial.println("New file announced:");
        Serial.println(filename);

        
        if(millis() - lastSyncRequest > 10000) // Sync cool down period
        {
            lastSyncRequest = millis();
        
            mesh.sendBroadcast(
                "FILE_LIST"
            );
        }



        return;
    }

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
  
      SimpleList<uint32_t> nodes = mesh.getNodeList();
  
      Serial.print("Nodes: ");
  
      for(auto node : nodes)
      {
          Serial.print(node);
          Serial.print(" ");
      }
  
      Serial.println();
  
  });


  // -----------------------
  // Web root
  // -----------------------

  server.on("/", HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    String html;

    html += "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body>";
    html += "<h1>Mesh File Sharing</h1>";
    

    Dir dir = LittleFS.openDir("/");


    while(dir.next())
    {
      String name = dir.fileName();

      html += "<p>";
      html += name;
      html += " (";
      html += dir.fileSize();
      html += " bytes) ";

      html += "<a href='/download?file=";
      html += name;
      html += "'>download</a>";

      html += "</p>";
    }


    html += "<hr>";

    html += "<h2>Upload (< 100 kb!)</h2>";

    html += "<form method='POST' action='/upload' "
            "enctype='multipart/form-data'>";

    html += "<input type='file' id='file' name='upload'>";

    html += "<input type='submit'>";

    html += "</form>";

    // Storage info 
    
    FSInfo fs_info;
    LittleFS.info(fs_info);
    
    float usedMB = fs_info.usedBytes / 1024.0 / 1024.0;
    float totalMB = fs_info.totalBytes / 1024.0 / 1024.0;
    float freeMB = (fs_info.totalBytes - fs_info.usedBytes) / 1024.0 / 1024.0;
    
    int percent = 
        (fs_info.usedBytes * 100) / fs_info.totalBytes;
    
    
    // ASCII progress bar
    int barWidth = 20;
    int filled = (percent * barWidth) / 100;
    
    String bar = "[";
    
    for(int i = 0; i < barWidth; i++)
    {
        if(i < filled)
            bar += "#";
        else
            bar += "-";
    }
    
    bar += "]";
    
    
    html += "<pre>";

    html += "Node ID: ";
    html += mesh.getNodeId();
    html += "\n\n";

    html += "Storage\n";
    html += bar;
    html += " ";
    html += percent;
    html += "%\n\n";
    
    html += "Used: ";
    html += String(usedMB,2);
    html += " MB / ";
    html += String(totalMB,2);
    html += " MB\n";
    
    html += "Free: ";
    html += String(freeMB,2);
    html += " MB";
    
    html += "</pre>";

    html += "<script>";
    
    html += "var uploadField = document.getElementById('file');";
    html += "uploadField.onchange = function() { if(this.files[0].size >";
    html += MAX_UPLOAD_SIZE;
    html += ") {";
    html += "alert('File is too big!');this.value = '';};};";
    html += "</script>";
    html += "</body></html>";

    request->send(
      200,
      "text/html",
      html
    );

  });



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

  Serial.println("HTTP server started");



  // Set a custom hostname for the device
//  bool success = mesh.setHostname("mesh");
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
  dnsServer.processNextRequest();

  userScheduler.execute(); //send file chunks


  static unsigned long syncTimer=0;


  if(!sendingFile && !receivingFile)
  {
      if(millis()-syncTimer>30000)
      {
          syncTimer=millis();
  
          if(mesh.getNodeList().size()>0)
          {
              mesh.sendBroadcast("FILE_LIST"); 
              Serial.println("Requesting file list");
          }
      }
  }

//    static unsigned long last = 0;
//  
//    if (millis() - last > 3000)
//    {
//      last = millis();
//  
//      Serial.println("----");
//      Serial.print("Node ID: ");
//      Serial.println(mesh.getNodeId());
//  
//      Serial.print("Connections: ");
//      Serial.println(mesh.getNodeList().size());
//  
//      for(auto node : mesh.getNodeList())
//      {
//        Serial.print("Connected: ");
//        Serial.println(node);
//      }
//    }
}
