let myNode = "2882848806";

document.getElementById("myNode").innerHTML = myNode;


function addNode(id)
{
    let div=document.getElementById("nodes");

    let link=document.createElement("a");

    link.href="#";

    link.innerHTML=id;


    link.onclick=function()
    {
        loadNode(id,"/index.html");
        return false;
    };


    div.appendChild(link);

    div.appendChild(
        document.createElement("br")
    );
}



function loadNode(node,path)
{
    fetch("/mesh/"+node+path)
    .then(() =>
    {
        checkProxy(node);
    });
}



function checkProxy(node)
{
    fetch("/proxy-status")

    .then(r=>r.json())

    .then(data =>
    {
        if(data.status=="ready")
        {
            fetch("/proxy-data")

            .then(r=>r.text())

            .then(html =>
            {
                document.open();
                document.write(html);
                document.close();
            });
        }
        else
        {
            setTimeout(
                ()=>checkProxy(node),
                200
            );
        }
    });
}



addNode("2880712319"); //deleteme