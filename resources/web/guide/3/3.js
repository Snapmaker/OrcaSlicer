
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
	var checkbox = document.getElementById('agreeCheckbox');
	var errorTip = document.getElementById('errorTip');
	
	if (!checkbox.checked) {
		// 未勾选复选框，显示错误提示
		errorTip.style.display = 'block';
		return;
	}
	
	// 已勾选，隐藏错误提示并继续
	errorTip.style.display = 'none';
	SendPrivacyChoice("agree");
	window.location.href="../21/index.html";
}

function OnCheckboxChange()
{
	var checkbox = document.getElementById('agreeCheckbox');
	var errorTip = document.getElementById('errorTip');
	
	// 勾选时隐藏错误提示
	if (checkbox.checked) {
		errorTip.style.display = 'none';
	}
	// 取消勾选时不显示错误提示（只有点击按钮时才检查）
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
	
	
	if(m_Region === "Chinese Mainland") {
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

function OpenCrossBorderNotice()
{
	var noticeUrl = "";
	
	// 个人信息出境告知书链接
	if(m_Region === "Chinese Mainland") {
		noticeUrl = "https://www.snapmaker.cn/cross-border-notice";
	} else {
		noticeUrl = "https://www.snapmaker.com/cross-border-notice";
	}
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="common_openurl";
	tSend['url']=noticeUrl;
	tSend['local']=m_Region
	
	SendWXMessage( JSON.stringify(tSend) );
}