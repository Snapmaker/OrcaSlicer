//var TestData={"sequence_id":"0","command":"get_recent_projects","response":[{"path":"D:\\work\\Models\\Toy\\3d-puzzle-cube-model_files\\3d-puzzle-cube.3mf","time":"2022\/3\/24 20:33:10"},{"path":"D:\\work\\Models\\Art\\Carved Stone Vase - remeshed+drainage\\Carved Stone Vase.3mf","time":"2022\/3\/24 17:11:51"},{"path":"D:\\work\\Models\\Art\\Kity & Cat\\Cat.3mf","time":"2022\/3\/24 17:07:55"},{"path":"D:\\work\\Models\\Toy\\鐩村墤.3mf","time":"2022\/3\/24 17:06:02"},{"path":"D:\\work\\Models\\Toy\\minimalistic-dual-tone-whistle-model_files\\minimalistic-dual-tone-whistle.3mf","time":"2022\/3\/22 21:12:22"},{"path":"D:\\work\\Models\\Toy\\spiral-city-model_files\\spiral-city.3mf","time":"2022\/3\/22 18:58:37"},{"path":"D:\\work\\Models\\Toy\\impossible-dovetail-puzzle-box-model_files\\impossible-dovetail-puzzle-box.3mf","time":"2022\/3\/22 20:08:40"}]};

var m_HotModelList=null;
var deviceClickCount = 0;
var deviceClickTimer = null;

//Recent详情页面的状态
var Recent_Normal=1;
var Recent_BatchDelete=2;

var RecentPage_Mode=Recent_Normal;

function OnInit()
{
	//-----Official-----
    TranslatePage();

	SendMsg_GetLoginInfo();
	SendMsg_GetRecentFile();
	SendMsg_GetStaffPick();
	SendMsg_GetLocalDevices();

    Set_RecentFile_Delete_Checkbox_Event();

    $('#Menu_Batch').css('display','flex');
	$('#Menu_Clear').css('display','flex');
	
	$('#Menu_Delete').hide();
	$('#Menu_Cancel').hide();
	
	
	$('.FileCheckBox.FileCheckBox_checked').removeClass('FileCheckBox_checked');
	$('.FileCheckBox').hide();
	$('.FileMask').hide();
}

//------最佳打开文件的右键菜单功能----------
var RightBtnFilePath='';

var MousePosX=0;
var MousePosY=0;
var sImages = {};
 
function Set_RecentFile_MouseRightBtn_Event()
{
	$(".FileItem").mousedown(
		function(e)
		{			
			//FilePath
			RightBtnFilePath=$(this).attr('fpath');
			
			if(e.which == 3){
				//鼠标点击了右键+$(this).attr('ff') );
				ShowRecnetFileContextMenu();
			}else if(e.which == 2){
				//鼠标点击了中键
			}else if(e.which == 1){
				//鼠标点击了左键
                if(RecentPage_Mode !== Recent_BatchDelete)
				OnOpenRecentFile( encodeURI(RightBtnFilePath) );
			}
		});

	$(document).bind("contextmenu",function(e){
		//在这里书写代码，构建个性右键化菜单
		return true;
	});	
	
    $(document).mousemove( function(e){
		MousePosX=e.pageX;
		MousePosY=e.pageY;
		
		let ContextMenuWidth=$('#recnet_context_menu').width();
		let ContextMenuHeight=$('#recnet_context_menu').height();
	
		let DocumentWidth=$(document).width();
		let DocumentHeight=$(document).height();
		
		//$("#DebugText").text( ContextMenuWidth+' - '+ContextMenuHeight+'<br/>'+
		//					 DocumentWidth+' - '+DocumentHeight+'<br/>'+
		//					 MousePosX+' - '+MousePosY +'<br/>' );
	} );
	

	$(document).click( function(){		
		var e = e || window.event;
        var elem = e.target || e.srcElement;
        while (elem) {
			if (elem.id && elem.id == 'recnet_context_menu') {
                    return;
			}
			elem = elem.parentNode;
		}		
		
		$("#recnet_context_menu").hide();
	} );

	
}

function SetLoginPanelVisibility(visible) {
  var leftBoard = document.getElementById("LeftBoard");
  if (visible) {
    leftBoard.style.display = "block";
  } else {
    leftBoard.style.display = "none";
  }
}

function HandleStudio(pVal) {
    // connect and disconnect
    try {
        // 只解析sequenceId相关的判断
        if(typeof pVal === 'string') {
            const jsonData = JSON.parse(pVal);
            if(jsonData && jsonData.header && typeof jsonData.header.sequenceId !== 'undefined'){
                let sequenceId = jsonData.header.sequenceId;
                if(sequenceId == 202500 || sequenceId == 202501){
                    // 移除所有卡片的connecting类
                    const cards = document.querySelectorAll('.DeviceCard.connecting');
                    cards.forEach(card => {
                        card.classList.remove('connecting');
                    });
                }
            }
            return; // 处理完连接状态就返回
        }
    } catch(e) {
        console.error('Error parsing JSON:', e);
    }

    // 其他命令的处理保持不变
    let strCmd = pVal['command'];
    
    if (strCmd == "get_recent_projects") {
        ShowRecentFileList(pVal["response"]);
    } else if (strCmd == "studio_userlogin") {
        SetLoginInfo(pVal["data"]["avatar"], pVal["data"]["name"]);
    } else if (strCmd == "local_devices_arrived") {
        showLocalDevices(pVal["data"]);
    } else if (strCmd == "studio_useroffline") {
        SetUserOffline();
    } else if (strCmd == "studio_set_mallurl") {
        SetMallUrl(pVal["data"]["url"]);
    } else if (strCmd == "studio_clickmenu") {
        let strName = pVal["data"]["menu"];

        GotoMenu(strName);
    } else if (strCmd == "network_plugin_installtip") {
        let nShow = pVal["show"] * 1;

        if (nShow == 1) {
            $("#NoPluginTip").show();
            $("#NoPluginTip").css("display", "flex");
        } else {
            $("#NoPluginTip").hide();
        }
    } else if (strCmd == "modelmall_model_advise_get") {
        //alert('hot');
        if (m_HotModelList != null) {
            let SS1 = JSON.stringify(pVal["hits"]);
            let SS2 = JSON.stringify(m_HotModelList);

            if (SS1 == SS2) return;
        }

        m_HotModelList = pVal["hits"];
        ShowStaffPick(m_HotModelList);
    } else if (data.cmd === "SetLoginPanelVisibility") {
        SetLoginPanelVisibility(data.visible);
    }
}

function GotoMenu(strMenu) {
    let MenuList = $(".BtnItem");
    let nAll = MenuList.length;
    
    for(let n = 0; n < nAll; n++) {
        let OneBtn = MenuList[n];
        
        if($(OneBtn).attr("menu") == strMenu) {
            $(".BtnItem").removeClass("BtnItemSelected");            
            $(OneBtn).addClass("BtnItemSelected");
            
            // 隐藏所有board内容
            $("div[board]").hide();
            // 显示对应board内容
            $("div[board='" + strMenu + "']").show();
        }
    }
}

function SetLoginInfo( strAvatar, strName ) 
{
	$("#Login1").hide();
	
	$("#UserName").text(strName);
	
    let OriginAvatar=$("#UserAvatarIcon").prop("src");
	if(strAvatar!=OriginAvatar)
		$("#UserAvatarIcon").prop("src",strAvatar);
	else
	{
		//alert('Avatar is Same');
	}
	
	$("#Login2").show();
	$("#Login2").css("display","flex");
}

function SetUserOffline()
{
	$("#UserAvatarIcon").prop("src","img/c.jpg");
	$("#UserName").text('');
	$("#Login2").hide();	
	
	$("#Login1").show();
	$("#Login1").css("display","flex");
}

function SetMallUrl( strUrl )
{
	$("#MallWeb").prop("src",strUrl);
}


function ShowRecentFileList( pList )
{
	let nTotal=pList.length;
	
	let strHtml='';
	for(let n=0;n<nTotal;n++)
	{
		let OneFile=pList[n];
		
		let sPath=OneFile['path'];
		let sImg=OneFile["image"] || sImages[sPath];
		let sTime=OneFile['time'];
		let sName=OneFile['project_name'];
		sImages[sPath] = sImg;
		
		//let index=sPath.lastIndexOf('\\')>0?sPath.lastIndexOf('\\'):sPath.lastIndexOf('\/');
		//let sShortName=sPath.substring(index+1,sPath.length);
		
		let TmpHtml='<div class="FileItem"  fpath="'+sPath+'"  >'+
				'<a class="FileTip" title="'+sPath+'"></a>'+
				'<div class="FileImg" ><img src="'+sImg+'" onerror="this.onerror=null;this.src=\'img/d.png\';"  alt="No Image"  /></div>'+
				'<div class="FileName TextS1">'+sName+'</div>'+
				'<div class="FileDate">'+sTime+'</div>'+
                '<div class="FileMask"></div>'+
				'<div class="FileCheckBox"></div>'+
			    '</div>';
		
		strHtml+=TmpHtml;
	}
	
	$("#FileList").html(strHtml);	
	
    Set_RecentFile_MouseRightBtn_Event();
	UpdateRecentClearBtnDisplay();
    Set_RecentFile_Delete_Checkbox_Event();
}

function ShowRecnetFileContextMenu()
{
	$("#recnet_context_menu").offset({top: 10000, left:-10000});
	$('#recnet_context_menu').show();
	
	let ContextMenuWidth=$('#recnet_context_menu').width();
	let ContextMenuHeight=$('#recnet_context_menu').height();
	
    let DocumentWidth=$(document).width();
	let DocumentHeight=$(document).height();

	let RealX=MousePosX;
	let RealY=MousePosY;
	
	if( MousePosX + ContextMenuWidth + 24 >DocumentWidth )
		RealX=DocumentWidth-ContextMenuWidth-24;
	if( MousePosY+ContextMenuHeight+24>DocumentHeight )
		RealY=DocumentHeight-ContextMenuHeight-24;
	
	$("#recnet_context_menu").offset({top: RealY, left:RealX});
}

/*-------RecentFile MX Message------*/
function SendMsg_GetLoginInfo()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_login_info";
	
	SendWXMessage( JSON.stringify(tSend) );	
}


function SendMsg_GetRecentFile()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="get_recent_projects";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function SendMsg_GetLocalDevices() {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "get_local_devices";
    
    SendWXMessage(JSON.stringify(tSend));
}

function OnLoginOrRegister()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_login_or_register";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnClickModelDepot()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_modeldepot";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickNewProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_newproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnClickOpenProject()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_openproject";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnOpenRecentFile( strPath )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_open_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(strPath);
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function OnDeleteRecentFile( )
{
	//Clear in UI
	$("#recnet_context_menu").hide();
	
	let AllFile=$(".FileItem");
	let nFile=AllFile.length;
	for(let p=0;p<nFile;p++)
	{
		let pp=AllFile[p].getAttribute("fpath");
		if(pp==RightBtnFilePath)
			$(AllFile[p]).remove();
	}	
	
	UpdateRecentClearBtnDisplay();
	
	//Send Msg to C++
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_recentfile";
	tSend['data']={};
	tSend['data']['path']=RightBtnFilePath;
	
	SendWXMessage( JSON.stringify(tSend) );
}

function OnDeleteAllRecentFiles()
{
	$('#FileList').html('');
	UpdateRecentClearBtnDisplay();
	
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_delete_all_recentfile";
	
	SendWXMessage( JSON.stringify(tSend) );
}

function UpdateRecentClearBtnDisplay()
{
    let AllFile=$(".FileItem");
	let nFile=AllFile.length;	
	if( nFile>0 )
    {
        $("#Menu_Clear").show();
        $('#Menu_Batch').show();
    }
    else
    {
        $("#Menu_Clear").hide();
        $('#Menu_Batch').hide();
    }
}




function OnExploreRecentFile( )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_explore_recentfile";
	tSend['data']={};
	tSend['data']['path']=decodeURI(RightBtnFilePath);
	
	SendWXMessage( JSON.stringify(tSend) );	
	
	$("#recnet_context_menu").hide();
}

function OnLogOut()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="homepage_logout";
	
	SendWXMessage( JSON.stringify(tSend) );	
}

function BeginDownloadNetworkPlugin()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="begin_network_plugin_download";
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OutputKey(keyCode, isCtrlDown, isShiftDown, isCmdDown) {
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "get_web_shortcut";
	tSend['key_event'] = {};
	tSend['key_event']['key'] = keyCode;
	tSend['key_event']['ctrl'] = isCtrlDown;
	tSend['key_event']['shift'] = isShiftDown;
	tSend['key_event']['cmd'] = isCmdDown;

	SendWXMessage(JSON.stringify(tSend));
}

//-------------User Manual------------

function OpenWikiUrl( strUrl )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_wiki_open";
	tSend['data']={};
	tSend['data']['url']=strUrl;
	
	SendWXMessage( JSON.stringify(tSend) );	
}

//--------------Staff Pick-------
var StaffPickSwiper=null;
function InitStaffPick()
{
	if( StaffPickSwiper!=null )
	{
		StaffPickSwiper.destroy(true,true);
		StaffPickSwiper=null;
	}	
	
	StaffPickSwiper = new Swiper('#HotModel_Swiper.swiper', {
            slidesPerView : 'auto',
		    spaceBetween: 16,
			navigation: {
				nextEl: '.swiper-button-next',
				prevEl: '.swiper-button-prev',
			},
		    slidesPerView : 'auto',
		    slidesPerGroup : 3
//			autoplay: {
//				delay: 3000,
//				stopOnLastSlide: false,
//				disableOnInteraction: true,
//				disableOnInteraction: false
//			},
//			pagination: {
//				el: '.swiper-pagination',
//			},
//		    scrollbar: {
//                el: '.swiper-scrollbar',
//				draggable: true
//            }
			});
}

function SendMsg_GetStaffPick()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_advise_get";
	
	SendWXMessage( JSON.stringify(tSend) );
	
	setTimeout("SendMsg_GetStaffPick()",3600*1000*1);
}

function ShowStaffPick( ModelList )
{
	let PickTotal=ModelList.length;
	if(PickTotal==0)
	{
		$('#HotModelList').html('');
		$('#HotModelArea').hide();
		
		return;
	}
	
	let strPickHtml='';
	for(let a=0;a<PickTotal;a++)
	{
		let OnePickModel=ModelList[a];
		
		let ModelID=OnePickModel['design']['id'];
		let ModelName=OnePickModel['design']['title'];
		let ModelCover=OnePickModel['design']['cover']+'?image_process=resize,w_200/format,webp';
		
		let DesignerName=OnePickModel['design']['designCreator']['name'];
		let DesignerAvatar=OnePickModel['design']['designCreator']['avatar']+'?image_process=resize,w_32/format,webp';
		
		strPickHtml+='<div class="HotModelPiece swiper-slide"  onClick="OpenOneStaffPickModel('+ModelID+')" >'+
			    '<div class="HotModel_Designer_Info"><img src="'+DesignerAvatar+'" /><span class="TextS2">'+DesignerName+'</span></div>'+
				'	<div class="HotModel_PrevBlock"><img class="HotModel_PrevImg" src="'+ModelCover+'" /></div>'+
				'	<div  class="HotModel_NameText TextS1" title="'+ModelName+'">'+ModelName+'</div>'+
				'</div>';
	}
	
	$('#HotModelList').html(strPickHtml);
	InitStaffPick();
	$('#HotModelArea').show();
}

function OpenOneStaffPickModel( ModelID )
{
	//alert(ModelID);
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="modelmall_model_open";
	tSend['data']={};
	tSend['data']['id']=ModelID;
	
	SendWXMessage( JSON.stringify(tSend) );		
}

function OnAddDevice() {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_add_device";
    
    SendWXMessage(JSON.stringify(tSend));
}

function OnTestBrowser(){
	var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_test_browser";
    
    SendWXMessage(JSON.stringify(tSend));
}

//---------------Global-----------------
window.postMessage = HandleStudio;

function onDeviceManagementClick(e) {
    // 阻止事件冒泡,避免触发菜单切换
    e.stopPropagation();
    
    deviceClickCount++;
    
    // 清除之前的定时器
    if (deviceClickTimer) {
        clearTimeout(deviceClickTimer);
    }
    
    // 设置新的定时器,1秒后重置点击次数
    deviceClickTimer = setTimeout(() => {
        deviceClickCount = 0;
    }, 1000);

    // 检查是否达到三次点击
    if (deviceClickCount >= 3) {
        deviceClickCount = 0;
        clearTimeout(deviceClickTimer);
        OnTestBrowser();
    }
}

function showLocalDevices(devices) {
    const deviceList = document.getElementById('DeviceList');
    while (deviceList.children.length > 1) {
        deviceList.removeChild(deviceList.lastChild);
    }
    
    devices.forEach(device => {
        const deviceCard = document.createElement('div');
        deviceCard.className = 'DeviceCard';
        deviceCard.setAttribute('data-ip', device.ip);
        
        // 添加加载动画容器
        const spinner = document.createElement('div');
        spinner.className = 'LoadingSpinner';
        deviceCard.appendChild(spinner);
        
        // 添加删除按钮
        const deleteBtn = document.createElement('div');
        deleteBtn.className = 'DeviceDeleteBtn';
        deleteBtn.onclick = (e) => {
            e.stopPropagation();
            handleDeviceDelete(device);
        };
        deviceCard.appendChild(deleteBtn);
        
        // 添加状态指示器
        const statusDot = document.createElement('div');
        statusDot.className = `DeviceStatus ${device.connected ? 'connected' : 'disconnected'}`;
        statusDot.setAttribute('data-status', device.connected ? '已连接' : '未连接');
        deviceCard.appendChild(statusDot);
        
        // 创建图片容器
        const imgContainer = document.createElement('div');
        imgContainer.className = 'DeviceImgContainer';
        
        // 创建图片元素
        const img = document.createElement('img');
        img.className = 'DeviceImg';
        img.src = device.img || 'img/i3.png';
        img.onerror = function() {
            this.onerror = null;
            this.src = 'img/i3.png';
        };
        
        // 添加创建工程按钮
        const createProjectBtn = document.createElement('div');
        createProjectBtn.className = 'create-project-btn';
        createProjectBtn.textContent = '创建工程';
        createProjectBtn.onclick = (e) => {
            e.stopPropagation();
            createProject(device.dev_id);
        };
        
        // 创建设备名称元素
        const name = document.createElement('div');
        name.className = 'DeviceName TextS1';
        name.textContent = device.dev_name || 'Unknown Device';
        name.title = device.dev_name || 'Unknown Device';
        
        // 添加底部操作栏
        const actions = document.createElement('div');
        actions.className = 'DeviceActions';
        
        // 重命名按钮
        const renameBtn = document.createElement('div');
        renameBtn.className = 'DeviceAction';
        renameBtn.innerHTML = '<span>重命名</span>';
        renameBtn.onclick = (e) => {
            e.stopPropagation();
            OnRenameDevice(device);
        };
        
        // 切换机型按钮
        const switchBtn = document.createElement('div');
        switchBtn.className = 'DeviceAction';
        switchBtn.innerHTML = '<span>切换机型</span>';
        switchBtn.onclick = (e) => {
            e.stopPropagation();
            OnSwitchModel(device);
        };
        
        // 连接/断开按钮
        const connectBtn = document.createElement('div');
        connectBtn.className = 'DeviceAction';
        if (device.connected) {
            connectBtn.innerHTML = '<span>断开</span>';
            connectBtn.onclick = (e) => {
                e.stopPropagation();
                OnDisconnectDevice(device);
            };
        } else {
            connectBtn.innerHTML = '<span>连接</span>';
            connectBtn.onclick = (e) => {
                e.stopPropagation();
                OnConnectDevice(device);
            };
        }
        
        actions.appendChild(renameBtn);
        actions.appendChild(switchBtn);
        actions.appendChild(connectBtn);
        deviceCard.appendChild(actions);
        
        // 添加重命名输入框
        const renameInput = document.createElement('div');
        renameInput.className = 'RenameInput';
        const input = document.createElement('textarea');
        input.value = device.dev_name || 'Unknown Device';
        input.rows = 3;

        // 处理输入框事件
        input.onkeydown = (e) => {
            e.stopPropagation();
            if (e.key === 'Enter' && !e.shiftKey) {
                e.preventDefault();
                submitRename(device, input.value);
            } else if (e.key === 'Escape') {
                cancelRename(deviceCard);
            }
        };

        input.onblur = () => {
            cancelRename(deviceCard);
        };

        // 自动调整高度
        input.oninput = () => {
            input.style.height = 'auto';
            input.style.height = input.scrollHeight + 'px';
        };

        renameInput.appendChild(input);
        deviceCard.appendChild(renameInput);
        
        // 组装卡片
        imgContainer.appendChild(img);
        imgContainer.appendChild(createProjectBtn);
        deviceCard.appendChild(imgContainer);
        deviceCard.appendChild(name);
        deviceCard.appendChild(deleteBtn);
        deviceList.appendChild(deviceCard);
    });
}

// 设备操作函数
function OnRenameDevice(device) {
    const deviceCard = document.querySelector(`.DeviceCard[data-ip="${device.ip}"]`);
    if (!deviceCard) return;
    
    // 添加重命名状态类
    deviceCard.classList.add('renaming');
    
    // 获取并聚焦输入框
    const input = deviceCard.querySelector('.RenameInput textarea');
    if (input) {
        input.focus();
        input.select();
    }
}

function submitRename(device, newName) {
    if (!newName.trim()) return;
    
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_rename_device";
    tSend['data'] = {
        ...device,
        dev_name: newName.trim()
    };
    SendWXMessage(JSON.stringify(tSend));
    
    // 移除重命名状态
    const deviceCard = document.querySelector(`.DeviceCard[data-ip="${device.ip}"]`);
    if (deviceCard) {
        deviceCard.classList.remove('renaming');
    }
}

function cancelRename(deviceCard) {
    deviceCard.classList.remove('renaming');
}

function OnSwitchModel(device) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "homepage_switch_model";
    tSend['data'] = device;
    SendWXMessage(JSON.stringify(tSend));
}

function OnConnectDevice(device) {
    // 找到对应的设备卡片并添加connecting类
    const deviceCard = document.querySelector(`.DeviceCard[data-ip="${device.ip}"]`);
    if (deviceCard) {
        deviceCard.classList.add('connecting');
    }
    
    body = {
        header:{
            sequenceId: 202501,
        },
        payload:{
            cmd: "sw_Connect",
            params: {
                dev_id: device.dev_id,
                ip: device.ip,
                sn: device.sn,
                user: device.user,
                password: device.password,
                ca: device.ca,
                cert: device.cert,
                key: device.key,
                clientid: device.clientid,
                port: device.port,
                from_homepage: true,
            }
        }
    }
    window.wx.postMessage(JSON.stringify(body));
}

function OnDisconnectDevice(device) {
	body = {
        header:{
          sequenceId: 202500,
        },
        payload:{
          cmd: "sw_Disconnect",
        }
      }
    window.wx.postMessage(JSON.stringify(body));
}

// 创建并显示对话框
function showDialog(title, content, onConfirm, onCancel) {
    // 创建遮罩层
    const overlay = document.createElement('div');
    overlay.className = 'ModalOverlay';
    
    // 创建对话框
    const dialog = document.createElement('div');
    dialog.className = 'DialogBox';
    
    // 添加标题
    const titleDiv = document.createElement('div');
    titleDiv.className = 'DialogTitle';
    titleDiv.textContent = title;
    
    // 添加内容
    const contentDiv = document.createElement('div');
    contentDiv.className = 'DialogContent';
    contentDiv.textContent = content;
    
    // 添加按钮
    const buttonsDiv = document.createElement('div');
    buttonsDiv.className = 'DialogButtons';
    
    // 取消按钮
    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'DialogButton cancel';
    cancelBtn.textContent = '取消';
    cancelBtn.onclick = () => {
        document.body.removeChild(overlay);
        if (onCancel) onCancel();
    };
    
    // 确认按钮
    const confirmBtn = document.createElement('button');
    confirmBtn.className = 'DialogButton confirm';
    confirmBtn.textContent = '确认';
    confirmBtn.onclick = () => {
        document.body.removeChild(overlay);
        if (onConfirm) onConfirm();
    };
    
    // 组装对话框
    buttonsDiv.appendChild(cancelBtn);
    buttonsDiv.appendChild(confirmBtn);
    dialog.appendChild(titleDiv);
    dialog.appendChild(contentDiv);
    dialog.appendChild(buttonsDiv);
    overlay.appendChild(dialog);
    
    // 显示对话框
    document.body.appendChild(overlay);
}

// 修改设备删除处理函数
function handleDeviceDelete(device) {
    if (device.connected) {
        showDialog(
            '断开连接确认',
            '该设备当前处于连接状态，需要先断开连接才能删除。是否断开连接？',
            () => {
                OnDisconnectDevice(device);
            }
        );
    } else {
        confirmAndDeleteDevice(device);
    }
}

function confirmAndDeleteDevice(device) {
    showDialog(
        '删除设备确认',
        '确定要删除该设备吗？删除后需要重新添加才能使用。',
        () => {
            var tSend = {};
            tSend['sequence_id'] = Math.round(new Date() / 1000);
            tSend['command'] = "homepage_delete_device";
            tSend['data'] = device;
            SendWXMessage(JSON.stringify(tSend));
        }
    );
}

function createProject(deviceId) {
    var tSend = {};
    tSend['sequence_id'] = Math.round(new Date() / 1000);
    tSend['command'] = "create_project";
    tSend['data'] = {
        "dev_id": deviceId
    };
    SendWXMessage(JSON.stringify(tSend));
}

function OnBatchDelete()
{
	//切换页面工作模式
	RecentPage_Mode=Recent_BatchDelete;
	
	$('#Menu_Batch').hide();
	$('#Menu_Clear').hide();
	
	$('#Menu_Delete').css('display','flex');
	$('#Menu_Cancel').css('display','flex');

	$('.FileCheckBox.FileCheckBox_checked').removeClass('FileCheckBox_checked');
	$('.FileCheckBox').show();	
}

function OnCancelDelete()
{
	//切换页面工作模式
	RecentPage_Mode=Recent_Normal;
	
	$('#Menu_Batch').css('display','flex');
	$('#Menu_Clear').css('display','flex');
	
	$('#Menu_Delete').hide();
	$('#Menu_Cancel').hide();
	
	
	$('.FileCheckBox.FileCheckBox_checked').removeClass('FileCheckBox_checked');
	$('.FileCheckBox').hide();
	$('.FileMask').hide();
}

function OnMultiDelete()
{
	let ChooseFiles=$('.FileCheckBox.FileCheckBox_checked');
	let nChoose=ChooseFiles.length;

	var tBatchDel={};
	tBatchDel['sequence_id']=Math.round(new Date() / 1000);
	tBatchDel['command']="homepage_delete_recentfile";
	tBatchDel['data']={};	
	
	for(let n=0;n<nChoose;n++)
	{
		let OneItem=ChooseFiles[n];
		let ParentItem=$(OneItem).parent();
		
		let fPath=$(ParentItem).attr("fpath");

		//删除文件对象
		$(ParentItem).remove();
		
		//发送WX消息
	    tBatchDel['data']['path']=fPath;	
   	    SendWXMessage( JSON.stringify(tBatchDel) );		
	}
	
	//更新按钮状态
	OnCancelDelete();	
	UpdateRecentClearBtnDisplay();	
}

function Set_RecentFile_Delete_Checkbox_Event()
{
	$(".FileCheckBox").mousedown(
		function(e)
		{			
			//FilePath		
			if(e.which == 3){
				//鼠标点击了右键+$(this).attr('ff') );
			}else if(e.which == 2){
				//鼠标点击了中键
			}else if(e.which == 1){
				//鼠标点击了左键
				if( $(this).hasClass('FileCheckBox_checked') )
				{
					$(this).removeClass('FileCheckBox_checked');
					$(this).prev('.FileMask').hide();
				}
				else
				{
					$(this).addClass('FileCheckBox_checked');
					$(this).prev('.FileMask').show();					
				}

                e.stopPropagation();
			}
		});	
}