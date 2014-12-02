/**
 * Model class
 * 
 * @returns {DashboardConfig}
 */
function DashboardConfig(agocontrol)
{
    var self = this;
    self.agocontrol = agocontrol;
    self.newDashboardName = ko.observable('');

    //filter dashboard that don't need to be displayed
    //need to do that because datatable odd is broken when filtering items using knockout
    self.dashboards = ko.computed(function()
    {
        var dashboards = [];
        for( var i=0; i<self.agocontrol.dashboards().length; i++ )
        {
            if( self.agocontrol.dashboards()[i].editable )
            {
                dashboards.push(self.agocontrol.dashboards()[i]);
            }
        }
        return dashboards;
    });

    self.makeEditable = function(item, td, tr)
    {
        if( $(td).hasClass('edit_dashboard') )
        {
            $(td).editable(
                function(value, settings)
                {
                    var content = {};
                    content.floorplan = item.uuid;
                    content.uuid = self.agocontrol.agoController;
                    content.command = "setfloorplanname";
                    content.name = value;
                    self.agocontrol.sendCommand(content);
                    return value;
                },
                {
                    data : function(value, settings)
                    {
                        return value;
                    },
                    onblur : "cancel"
                }
            ).click();
        }
    };

    self.grid = new ko.agoGrid.viewModel({
        data: self.dashboards,
        columns: [
            {headerText:'Name', rowText:'name'},
            {headerText:'Actions', rowText:''}
        ],
        rowCallback: self.makeEditable,
        rowTemplate: 'rowTemplate',
        displaySearch: false,
        displayPagination: false,
        displayRowCount: true
    });

    self.deletePlan = function(item, event)
    {
        var button_yes = $("#confirmDeleteButtons").data("yes");
        var button_no = $("#confirmDeleteButtons").data("no");
        var buttons = {};
        buttons[button_no] = function()
        {
            $("#confirmDelete").dialog("close");
        };
        buttons[button_yes] = function()
        {
            self.doDeletePlan(item, event);
            $("#confirmDelete").dialog("close");
        };
        $("#confirmDelete").dialog({
            modal : true,
            height : 180,
            width : 500,
            buttons : buttons
        });
    };

    self.doDeletePlan = function(item, event)
    {
        self.agocontrol.block($('#agoGrid'));
        var content = {};
        content.floorplan = item.uuid;
        content.uuid = self.agocontrol.agoController;
        content.command = 'deletefloorplan';
        self.agocontrol.sendCommand(content, function(res)
        {
            if (res.result && res.result.returncode == 0)
            {
                self.agocontrol.refreshDashboards();
            }
            else
            {
                notif.error("Error while deleting dashboard!");
            }
            self.agocontrol.unblock($('#agoGrid'));
        });
    };

    self.createDashboard = function()
    {
        if( $.trim(self.newDashboardName())!='' )
        {
            self.agocontrol.block($('#agoGrid'));
            var content = {};
            content.command = "setfloorplanname";
            content.uuid = self.agocontrol.agoController;
            content.name = self.newDashboardName();
            self.agocontrol.sendCommand(content, function(response)
            {
                self.newDashboardName('');
                self.agocontrol.refreshDashboards();
                self.agocontrol.unblock($('#agoGrid'));
            });
        }
    };
}

/**
 * Initalizes the model
 */
function init_template(path, params, agocontrol)
{
    model = new DashboardConfig(agocontrol);
    return model;
}
