// Setup wizard: printer page (Back -> region, Next -> filaments).
// The table itself is ../js/printer_table.js (shared with the Printer Selection dialog, page 24).
// What it sends is the same as the dialog: the enabled models keyed by vendor + model, each with
// all its nozzle variants.

function OnInit()
{
	TranslatePage();
	PrinterTable.init(document.getElementById('PtRoot'));
	RequestProfile();
}

function RequestProfile()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "request_userguide_profile";

	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio(pVal)
{
	let strCmd = pVal['command'];

	if (strCmd == 'response_userguide_profile')
		PrinterTable.load(pVal['response']);
}

function GotoFilamentPage()
{
	let nChoose = OnExitFilter();

	if (nChoose > 0)
		window.open('../22/index.html', '_self');
	else
		ShowNotice(1);
}

// Sends the selection to the slicer; returns how many printer models are enabled.
function OnExitFilter()
{
	let sel = PrinterTable.selection();

	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "save_userguide_models";
	tSend['data'] = sel.data;

	SendWXMessage(JSON.stringify(tSend));

	return sel.count;
}

function ShowNotice(nShow)
{
	if (nShow == 0) {
		$("#NoticeMask").hide();
		$("#NoticeBody").hide();
	}
	else {
		$("#NoticeMask").show();
		$("#NoticeBody").show();
	}
}

document.addEventListener('keydown', function (e) {
	if (e.key === 'Escape') {
		if (!PrinterTable.closePopover() && $("#NoticeBody").is(":visible"))
			ShowNotice(0);
	}
	else if ((e.key === 'Enter' || e.key === ' ') && e.target && e.target.getAttribute && e.target.getAttribute('role') === 'button' && e.target.onclick) {
		e.preventDefault();
		e.target.onclick();
	}
});

// Ctrl + wheel would zoom the page.
window.addEventListener('wheel', function (event) {
	if (event.ctrlKey === true || event.metaKey)
		event.preventDefault();
}, { passive: false });
