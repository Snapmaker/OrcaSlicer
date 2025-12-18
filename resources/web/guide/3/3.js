
var m_Region = '';  

function OnInit()
{
	TranslatePage();
	
	
	RequestProfile();
}

function RequestProfile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_userguide_profile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{	
	let strCmd=pVal['command'];
	
	if(strCmd=='response_userguide_profile')
	{
		
		if(pVal['response'] && pVal['response']['region']) {
			m_Region = pVal['response']['region'];
		}
	}
}


function GotoBackPage()
{
	window.location.href="../11/index.html";
}


function GotoNextPage()
{
	
	SendPrivacyChoice("agree");
	
	
	window.location.href="../21/index.html";
}


function GotoSkipPage()
{
	
	SendPrivacyChoice("refuse");
	
	
	window.location.href="../21/index.html";
}


function SendPrivacyChoice(action)
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_private_choice";  
	tSend['data']={};
	tSend['data']['action']=action;  // "agree" 或 "refuse"
	
	SendWXMessage( JSON.stringify(tSend) );
}


function OpenPrivacyPolicy()
{
	var privacyUrl = "";
	
	
	if(m_Region === "China") {
		privacyUrl = "https://www.snapmaker.cn/privacy-policy.html";
	} else {
		privacyUrl = "https://www.snapmaker.com/privacy-policy";
	}
	
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="common_openurl";
	tSend['url']=privacyUrl;
	tSend['local']=m_Region
	
	SendWXMessage( JSON.stringify(tSend) );
}
