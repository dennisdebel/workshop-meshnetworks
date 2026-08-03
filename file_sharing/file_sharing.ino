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
// Globals
// -----------------------

painlessMesh mesh;

AsyncWebServer server(80);

Scheduler userScheduler;

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

// HTML line builder
String boxLine(String content, int width)
{
    String line = "| ";

    line += content;

    while(line.length() < width - 1)
    {
        line += " ";
    }

    line += "|";

    return line;
}

// HTML aware line builder
String boxLineHTML(String content, int visibleLength, int width)
{
    String line = "| ";

    line += content;

    int spaces = width - visibleLength - 3;

    for(int i = 0; i < spaces; i++)
    {
        line += "&nbsp;";
    }

    line += "|";

    return line;
}

// HTML progress bar 
String progressBar(int percent, int width)
{
    String bar = "[";

    int filled = (percent * width) / 100;

    for(int i=0;i<width;i++)
    {
        if(i < filled)
            bar += "#";
        else
            bar += "-";
    }

    bar += "]";

    return bar;
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

            
            if(isSyncIgnored(filename)) // ignore local files
              continue;
        
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

        if(isSyncIgnored(filename)) 
        {
            Serial.println("Ignoring LOCAL file");
            return;
        }
    
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

  int boxWidth = 36;
  String border = "+----------------------------------+";
  
  server.on("/", HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    String html;


    html += "<html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>";
    
    html += "body {";
    html += "background-color:#000;";
    html += "background-image:url('/download?file=LOCAL_background.jpg');";
    html += "background-position:center;";
    html += "background-size:cover;";
    html += "background-attachment:fixed;";
    html += "background-repeat:no-repeat;";
    html += "color:#fff;";
    html += "font-family:monospace;";
    html += "font-size:16px;";
    html += "margin:0;";
    html += "padding:10px;";
    html += "}";
      
    html += "pre {";
    html += "font-family: monospace;";
    html += "white-space: pre;";
    html += "overflow-x:auto;";
    html += "}";
    
    html += "input[type=file] {";
    html += "color:white;";
    html += "font-family:monospace;";
    html += "}";
        
    html += ".asciiButton {";
    html += "background:rgba(0,0,0,0.45);";
    html += "backdrop-filter:blur(5px);";
    html += "-webkit-backdrop-filter:blur(5px);";
    html += "color:white;";
    html += "border:0;";
    html += "font-family:monospace;";
    html += "font-size:16px;";
    html += "cursor:pointer;";
    html += "padding:0;";
    html += "margin:0;";
    html += "}";

    // remove ghost newlines
    html += "form {";
    html += "display:inline;";
    html += "margin:0;";
    html += "padding:0;";
    html += "}";

    html += "label.asciiButton {";
    html += "display:inline;";
    html += "}";
    
    
    html += "button.asciiButton {";
    html += "appearance:none;";
    html += "-webkit-appearance:none;";
    html += "}";

    html += "a {";
    html += "color:white;";
    html += "}";
    
    html += ".box {";
    html += "font-family:monospace;";
    html += "white-space:pre;";
    html += "width:38ch;";
    html += "overflow:hidden;";
    html += "}";

    html += "</style>";
    html += "</head><body>";

    // -----------------------
    // Calculate storage
    // -----------------------

    FSInfo fs_info;
    LittleFS.info(fs_info);

    float usedMB =
        fs_info.usedBytes / 1024.0 / 1024.0;

    float totalMB =
        fs_info.totalBytes / 1024.0 / 1024.0;

    float freeMB =
        (fs_info.totalBytes - fs_info.usedBytes)
        /1024.0/1024.0;

    int percent =
        (fs_info.usedBytes * 100)
        / fs_info.totalBytes;

    int barWidth = 25;

    int filled =
        (percent * barWidth) / 100;

    String bar="[ ";

    for(int i=0;i<barWidth;i++)
    {
        if(i < filled)
            bar += "#";
        else
            bar += "-";
    }
    bar += " ]";

    // -----------------------
    // Header / identity
    // -----------------------

    html += "<pre>";


    html += "+------------[ MESH NODE ]-----------+\n";

    html += "\n";
    
    html += "</pre>";
    
    // -----------------------
    // Identity
    // -----------------------
    
    html += "<pre id='identity'>";
    
    html += "+------------[ identity ]------------+\n";
    
    
    String nodeLine;
    nodeLine = "node id: ";
    nodeLine += mesh.getNodeId();
    
    html += boxLineHTML(
        nodeLine,
        nodeLine.length(),
        38
    );
    html += "\n";
    
    
    String heapLine;
    heapLine = "heap: ";
    heapLine += ESP.getFreeHeap();
    heapLine += " bytes";
    
    html += boxLineHTML(
        heapLine,
        heapLine.length(),
        38
    );
    html += "\n";
    
    
    String peerLine;
    peerLine = "peers: ";
    peerLine += mesh.getNodeList().size();
    
    html += boxLineHTML(
        peerLine,
        peerLine.length(),
        38
    );
    html += "\n";
    
    
    html += "+------------------------------------+\n";
    
    html += "</pre>";
    
    html += "\n";


    // -----------------------
    // Files
    // -----------------------

    html += "<div id='files'>";
    html += "<pre>";

    html += "+------------------------------------+\n";
    html += boxLine("[ files ]", 38);
    html += "\n";
    
    html += boxLine("", 38); // Give some space to the title
    html += "\n";
    
    Dir dir = LittleFS.openDir("/");

    while(dir.next())
    {
        String name = dir.fileName();
    
        if(name.endsWith(".tmp"))
            continue;
    
    
        String checkName = name;
    
        if(checkName.startsWith("/"))
        {
            checkName = checkName.substring(1);
        }
    
    
        if(checkName.startsWith("LOCAL_"))
            continue;
    
    
        String fileEntry;
    
        fileEntry = name;
    
        fileEntry += " <a href='/download?file=";
        fileEntry += name;
        fileEntry += "'>[download]</a>";
   
        
        int visibleLength = name.length() + 11;
    
    
        html += boxLineHTML(
            fileEntry,
            visibleLength,
            38
        );
            
        Serial.println(html);
        
        html += "\n";
    }
    
    
    html += "+------------------------------------+\n";
    
    html += "</pre>";
    html += "</div>";




    // -----------------------
    // Disk
    // -----------------------
    
    html += "<pre>";
    
    html += "+------------------------------------+\n";
    
    html += boxLine("[ disk space ]",38);
    html += "\n";
    
    html += boxLine("",38);
    html += "\n";
    
    
    html += boxLine(bar + " " + String(percent) + "%",38);
    html += "\n";
    
    
    String usedLine;
    
    usedLine = "used: ";
    usedLine += String(usedMB,2);
    usedLine += " MB / ";
    usedLine += String(totalMB,2);
    usedLine += " MB";
    
    html += boxLine(usedLine,38);
    html += "\n";
    
    
    String freeLine;
    
    freeLine = "free: ";
    freeLine += String(freeMB,2);
    freeLine += " MB";
    
    html += boxLine(freeLine,38);
    html += "\n";
    
    
    html += "+------------------------------------+\n";
    
    html += "</pre>";

    
    // -----------------------
    // Upload
    // -----------------------
    
    html += "<div id='upload'><pre>";
    
    html += "+------------------------------------+\n";
    html += boxLine("[ upload ]",38);
    html += "\n";

    html += boxLine("",38);
    html += "\n";
    
    html += "<form method='POST' action='/upload' ";
    html += "enctype='multipart/form-data'>";
    
    
    // hidden real picker
    html += "<input type='file' id='file' name='upload' style='display:none;'>";
    
    
    // choose button line
    String chooseLine;
    
    chooseLine = "<label for='file' class='asciiButton'>[ choose file ]</label>";
    
    html += boxLineHTML(
        chooseLine,
        15, // 15 almost....
        38
    );
    
    html += "\n";
    
    
    // filename line
    String fileLine;
    
    fileLine = "file: ";
    fileLine += "<span id='filename'>none</span>";
    
    html += boxLineHTML(
        fileLine,
        10, // PERFECT
        38
    );
    
    html += "\n";
    
    
    // upload button line
    String uploadLine;
    
    uploadLine = "<button class='asciiButton' type='submit'>";
    uploadLine += "[ upload ]";
    uploadLine += "</button>";
    
    html += boxLineHTML(
        uploadLine,
        10,
        38
    );
 
       
    html += "</form>";
    
    html += "\n";
    html += boxLine("",38);
    html += "\n";
    
    html += "+------------------------------------+\n";

    html += "</pre></div>";

    // -----------------------
    // Transfer status
    // -----------------------
    
    html += "</pre>";

    html += "<pre id='transfer'>";
    
    html += "+------------------------------------+\n";
    html += "| [ transfer status ]                |\n";
    html += "|                                    |\n";
    html += "| loading...                        |\n";
    html += "+------------------------------------+\n";
    
    html += "</pre>";

    // -----------------------
    // Javascript
    // -----------------------

    html += "<script>";

    html += "function updateIdentity(){";
    html += "fetch('/identity')";
    html += ".then(r=>r.text())";
    html += ".then(t=>{";
    html += "document.getElementById('identity').textContent=t;";
    html += "});";
    html += "}";
    
    html += "setInterval(updateIdentity,3000);";
    html += "updateIdentity();";
    
    html += "</script>";

    html += "<script>";
    html += "function updateStatus(){";
    html += "fetch('/status')"; 
    html += ".then(r=>r.text())";
    html += ".then(t=>{";
    html += "document.getElementById('transfer').textContent=t;";
    html += "});";
    html += "}";
    
    html += "setInterval(updateStatus,2000);";
    html += "updateStatus();";
    html += "</script>";

    html += "<script>";

    html += "function updateFiles(){";
    html += "fetch('/files')";
    html += ".then(r=>r.text())";
    html += ".then(t=>{";
    html += "document.getElementById('files').innerHTML=t;";
    html += "});";
    html += "}";
    
    html += "setInterval(updateFiles,4000);";
    html += "updateFiles();";
    
    html += "</script>";

    html += "<script>";

    html += "var f=document.getElementById('file');";

    html += "f.onchange=function(){";

    html += "document.getElementById('filename').innerHTML=this.files[0].name;";


    html += "if(this.files[0].size > 100000){";

    html += "alert('File too big!');";

    html += "this.value='';";

    html += "}";


    html += "};";


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

   // Peer list 
   server.on("/identity", HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
    
        String out;
    
    
        out += "+------------[ identity ]------------+\n";
    
    
        String nodeLine;
        nodeLine = "node id: ";
        nodeLine += mesh.getNodeId();
    
        out += boxLine(
            nodeLine,
            38
        );
    
        out += "\n";
    
    
        String heapLine;
        heapLine = "heap: ";
        heapLine += ESP.getFreeHeap();
        heapLine += " bytes";
    
        out += boxLine(
            heapLine,
            38
        );
    
        out += "\n";
    
    
        String peerLine;
        peerLine = "peers: ";
        peerLine += mesh.getNodeList().size();
    
        out += boxLine(
            peerLine,
            38
        );
    
        out += "\n";
    
    
        out += "+------------------------------------+";
    
    
        request->send(
            200,
            "text/plain",
            out
        );
    
    });

   
   // Status page
  server.on("/status", HTTP_GET,
  [](AsyncWebServerRequest *request)
  {

    String out;


    out += "+------------------------------------+\n";


    out += boxLine("[ transfer status ]", 38);
    out += "\n";


    if(sendingFile)
    {

        String line;

        line = "TX: ";
        line += outgoingFilename;

        out += boxLine(line, 38);
        out += "\n";


        int percent = 0;

        if(outgoingFile.size() > 0)
        {
            percent =
            (outgoingPos * 100)
            / outgoingFile.size();
        }


        line = progressBar(percent,25);
        line += " ";
        line += percent;
        line += "%";


        out += boxLine(line,38);
        out += "\n";

    }
    else if(receivingFile)
    {

        String line;

        line = "RX: ";
        line += incoming.name;


        out += boxLine(line,38);
        out += "\n";


        int percent=0;

        if(incoming.size > 0)
        {
            percent =
            (incoming.received * 100)
            / incoming.size;
        }


        line = progressBar(percent,25);
        line += " ";
        line += percent;
        line += "%";


        out += boxLine(line,38);
        out += "\n";

    }
    else
    {

        out += boxLine("idle",38);
        out += "\n";

    }


    out += "+------------------------------------+";


    request->send(
        200,
        "text/plain",
        out
    );

  });


    // Update file list on web interface on download
    server.on("/files", HTTP_GET,
    [](AsyncWebServerRequest *request)
    {
    
        String out;

        out += "<pre>";
        
        out += "+------------------------------------+\n";
        out += boxLine("[ files ]",38);
        out += "\n";
    
        Dir dir = LittleFS.openDir("/");

        while(dir.next())
{
    String name = dir.fileName();

    if(name.endsWith(".tmp"))
        continue;

    if(name.startsWith("LOCAL_"))
        continue;

    String entry;

    entry = name;

    entry += " <a href='/download?file=";
    entry += name;
    entry += "'>[download]</a>";

    int visibleLength = name.length() + 11;

    out += boxLineHTML(
        entry,
        visibleLength,
        38
    );

    out += "\n";
}   // <-- THIS closes while


out += "+------------------------------------+\n";

out += "</pre>";

request->send(
    200,
    "text/html",
    out
);

}); 
          
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
}
