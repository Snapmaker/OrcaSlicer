// Printer Selection dialog (sidebar printer list -> "Select/Remove printers (system presets)").
// The table itself is ../js/printer_table.js (shared with the wizard's page 21); this page adds
// Confirm / Cancel. What it sends is unchanged from the tile page (#345): the enabled models keyed
// by vendor + model, each with all its nozzle variants, then "user_guide_finish".

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

// Sends the selection to the slicer (it only updates the dialog's data; nothing is saved
// until "user_guide_finish"). Returns how many printer models are enabled.
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

function CancelSelect()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "user_guide_cancel";
	tSend['data'] = {};

	SendWXMessage(JSON.stringify(tSend));
}

function ConfirmSelect()
{
	let nChoose = OnExitFilter();

	if (nChoose > 0) {
		var tSend = {};
		tSend['sequence_id'] = Math.round(new Date() / 1000);
		tSend['command'] = "user_guide_finish";
		tSend['data'] = {};
		tSend['data']['action'] = "finish";

		SendWXMessage(JSON.stringify(tSend));
	}
	else
		ShowNotice(1);
}

function ClosePage()
{
	var tSend = {};
	tSend['sequence_id'] = Math.round(new Date() / 1000);
	tSend['command'] = "close_page";
	SendWXMessage(JSON.stringify(tSend));
}

document.addEventListener('keydown', function (e) {
	if (e.key === 'Escape') {
		// Esc first closes an open filter or the notice, then the dialog.
		if (PrinterTable.closePopover())
			return;
		if ($("#NoticeBody").is(":visible")) {
			ShowNotice(0);
			return;
		}
		ClosePage();
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
