/**
 * Model class
 * 
 * @returns {ScenarioConfig}
 */
function ScenarioConfig(agocontrol)
{
    var self = this;
    self.agocontrol = agocontrol;
    self.scenarioName = ko.observable('');
    this.scenarios = ko.computed(function() {
        return self.agocontrol.devices().filter(function(d) {
            return d.devicetype=='scenario';
        });
    });

    self.makeEditable = function(item, td, tr)
    {
        if( $(td).hasClass('edit_scenario') )
        {
            $(td).editable(
                function(value, settings)
                {
                    var content = {};
                    content.device = item.uuid;
                    content.uuid = self.agocontrol.agoController;
                    content.command = "setdevicename";
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
        else if( $(td).hasClass('select_room') )
        {
            $(td).editable(
                function(value, settings)
                {
                    var content = {};
                    content.device = $(this).data('uuid');
                    content.uuid = self.agocontrol.agoController;
                    content.command = "setdeviceroom";
                    content.room = value == "unset" ? "" : value;
                    self.agocontrol.sendCommand(content);
                    var name = "unset";
                    for( var i=0; i<self.agocontrol.rooms().length; i++ )
                    {
                        if( self.agocontrol.rooms()[i].uuid==value )
                        {
                            name = self.agocontrol.rooms()[i].name;
                        }
                    }
                    return value == "unset" ? "unset" : name;
                },
                {
                    data : function(value, settings)
                    {
                        var list = {};
                        list["unset"] = "--";
                        for( var i=0; i<self.agocontrol.rooms().length; i++ )
                        {
                            list[self.agocontrol.rooms()[i].uuid] = self.agocontrol.rooms()[i].name;
                        }
                        return JSON.stringify(list);
                    },
                    type : "select",
                    onblur : "submit"
                }
            ).click();
        }
    };

    self.grid = new ko.agoGrid.viewModel({
        data: self.scenarios,
        columns: [
            {headerText:'Name', rowText:'name'},
            {headerText:'Room', rowText:'room'},
            {headerText:'Actions', rowText:''}
        ],
        rowCallback: self.makeEditable,
        rowTemplate: 'rowTemplate'
    });

    //Creates a scenario map out of the form fields inside a container
    self.buildScenarioMap = function(containerID)
    {
        var map = {};
        var map_idx = 0;
        var commands = document.getElementById(containerID).childNodes;
        for ( var i = 0; i < commands.length; i++)
        {
            var command = commands[i];
            var tmp = {};
            for ( var j = 0; j < command.childNodes.length; j++)
            {
                var child = command.childNodes[j];
                if (child.name && child.name == "device" && child.options[child.selectedIndex].value != "sleep")
                {
                    tmp.uuid = child.options[child.selectedIndex].value;
                }
                else if (child.tagName == "DIV")
                {
                    for ( var k = 0; k < child.childNodes.length; k++)
                    {
                        var subChild = child.childNodes[k];
                        if (subChild.name && subChild.name == "command")
                        {
                            tmp.command = subChild.options[subChild.selectedIndex].value;
                        }
                        if (subChild.name && subChild.type && subChild.type == "text")
                        {
                            tmp[subChild.name] = subChild.value;
                        }
                    }
                }
            }
            map[map_idx++] = tmp;
        }

        return map;
    };

    //Sends the create scenario command
    self.createScenario = function()
    {
        if( $.trim(self.scenarioName())=='' )
        {
            notif.warning("Please supply an event name!");
            return;
        }

        self.agocontrol.block($('#agoGrid'));

        var content = {};
        content.command = "setscenario";
        content.uuid = self.agocontrol.scenarioController;
        content.scenariomap = self.buildScenarioMap("scenarioBuilder");
        self.agocontrol.sendCommand(content, function(res)
        {
            if (res.result && res.result.scenario)
            {
                var cnt = {};
                cnt.uuid = self.agocontrol.agoController;
                cnt.device = res.result.scenario;
                cnt.command = "setdevicename";
                cnt.name = self.scenarioName();
                self.agocontrol.sendCommand(cnt, function(nameRes) {
                    if (nameRes.result && nameRes.result.returncode == "0")
                    {
                        self.agocontrol.refreshDevices(false);
                        document.getElementById("scenarioBuilder").innerHTML = "";
                    }

                    self.agocontrol.unblock($('#agoGrid'));
                });
            }
            else
            {
                notif.warning("Please add commands before creating the scenario!");
            }
        });
    };

    //Adds a command selection entry
    self.addCommand = function(containerID, defaultValues)
    {
        var row = document.createElement("div");

        if (!containerID)
        {
            containerID = "scenarioBuilder";
        }

        var removeBtn = document.createElement("input");
        removeBtn.style.display = "inline";
        removeBtn.type = "button";
        removeBtn.value = "-";
        row.appendChild(removeBtn);

        removeBtn.onclick = function()
        {
            row.parentNode.removeChild(row);
        };

        var deviceSelect = document.createElement("select");
        deviceSelect.name = "device";
        deviceSelect.style.display = "inline";
        deviceSelect.options.length = 0;
        self.agocontrol.devices().sort(function(a, b) {
            return a.room.localeCompare(b.room);
        });
        for ( var i = 0; i < self.agocontrol.devices().length; i++)
        {
            var dev = self.agocontrol.devices()[i];
            if( self.agocontrol.schema().devicetypes[dev.devicetype] && self.agocontrol.schema().devicetypes[dev.devicetype].commands.length > 0 && dev.name)
            {
                var dspName = "";
                if (dev.room)
                {
                    dspName = dev.room + " - " + dev.name;
                }
                else
                {
                    dspName = dev.name;
                }
                deviceSelect.options[deviceSelect.options.length] = new Option(dspName, dev.uuid);
                deviceSelect.options[deviceSelect.options.length - 1]._dev = dev;
                if (defaultValues && defaultValues.uuid == dev.uuid)
                {
                    deviceSelect.selectedIndex = deviceSelect.options.length - 1;
                }
            }
        }

        // Special case for the sleep command
        deviceSelect.options[deviceSelect.options.length] = new Option("Sleep", "sleep");
        deviceSelect.options[deviceSelect.options.length - 1]._dev = "sleep";
        if (defaultValues && !defaultValues.uuid)
        {
            deviceSelect.selectedIndex = deviceSelect.options.length - 1;
        }

        row.appendChild(deviceSelect);

        var commandContainer = document.createElement("div");
        commandContainer.style.display = "inline";

        deviceSelect.onchange = function()
        {
            commandContainer.innerHTML = "";
            var dev = deviceSelect.options[deviceSelect.selectedIndex]._dev;
            var commands = document.createElement("select");
            commands.name = "command";
            if (dev != "sleep")
            {
                for ( var i = 0; i < self.agocontrol.schema().devicetypes[dev.devicetype].commands.length; i++)
                {
                    var cmd = self.agocontrol.schema().devicetypes[dev.devicetype].commands[i];
                    commands.options[i] = new Option(self.agocontrol.schema().commands[cmd].name, cmd);
                    commands.options[i]._cmd = self.agocontrol.schema().commands[cmd];
                    if (defaultValues && defaultValues.command == cmd)
                    {
                        commands.selectedIndex = i;
                    }
                }
            }
            else
            {
                // Special case for the sleep command
                commands.options[commands.options.length] = new Option("Delay", "scenariosleep");
                commands.options[commands.options.length - 1]._cmd = "sleep";
                if (defaultValues && defaultValues.command == "scenariosleep")
                {
                    commands.selectedIndex = commands.options.length - 1;
                }
            }
            commands.style.display = "inline";
            commandContainer.appendChild(commands);
            commands.onchange = function()
            {
                if (commandContainer._params)
                {
                    for ( var i = 0; i < commandContainer._params.length; i++)
                    {
                        try
                        {
                            commandContainer.removeChild(commandContainer._params[i]);
                        }
                        catch (e)
                        {
                            // ignore node is gone
                        }
                    }
                    commandContainer._params = null;
                }

                var cmd = commands.options[commands.selectedIndex]._cmd;
                if (cmd.parameters)
                {
                    commandContainer._params = [];
                    for ( var key in cmd.parameters)
                    {
                        var field = document.createElement("input");
                        field = document.createElement("input");
                        field.setAttribute("type", "text");
                        field.setAttribute("size", "15");
                        field.setAttribute("name", key);
                        field.setAttribute("placeholder", cmd.parameters[key].name);
                        if (defaultValues && defaultValues[key])
                        {
                            field.setAttribute("value", defaultValues[key]);
                        }
                        commandContainer._params.push(field);
                        commandContainer.appendChild(field);
                    }
                }
                else if (cmd == "sleep")
                {
                    // Special case for the sleep command
                    commandContainer._params = [];
                    var field = document.createElement("input");
                    field = document.createElement("input");
                    field.setAttribute("type", "text");
                    field.setAttribute("size", "20");
                    field.setAttribute("name", "delay");
                    field.setAttribute("placeholder", "Delay in seconds");
                    if (defaultValues && defaultValues["delay"])
                    {
                        field.setAttribute("value", defaultValues.delay);
                    }
                    commandContainer._params.push(field);
                    commandContainer.appendChild(field);
                }
            };

            if (commands.options.length > 0)
            {
                commands.onchange();
            }
        };

        deviceSelect.onchange();

        row.appendChild(commandContainer);

        // Move up button
        var upBtn = document.createElement("input");
        upBtn.style.display = "inline";
        upBtn.setAttribute("type", "button");
        upBtn.setAttribute("value", "\u21D1");

        upBtn.onclick = function()
        {
            var prev = row.previousSibling;
            document.getElementById(containerID).removeChild(row);
            document.getElementById(containerID).insertBefore(row, prev);
        };

        row.appendChild(upBtn);

        // Move down button
        var downBtn = document.createElement("input");
        downBtn.style.display = "inline";
        downBtn.setAttribute("type", "button");
        downBtn.setAttribute("value", "\u21D3");
        downBtn.onclick = function()
        {
            var next = row.nextSibling;
            document.getElementById(containerID).removeChild(next);
            document.getElementById(containerID).insertBefore(next, row);
        };

        row.appendChild(downBtn);

        document.getElementById(containerID).appendChild(row);
    };

    self.deleteScenario = function(item, event)
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
            self.doDeleteScenario(item, event);
            $("#confirmDelete").dialog("close");
        };
        $("#confirmDelete").dialog({
            modal : true,
            height : 180,
            width : 500,
            buttons : buttons
        });
    };

    //Sends the delete scenario command
    self.doDeleteScenario = function(item, event)
    {
        self.agocontrol.block($('#agoGrid'));
        var content = {};
        content.scenario = item.uuid;
        content.uuid = self.agocontrol.scenarioController;
        content.command = 'delscenario';
        self.agocontrol.sendCommand(content, function(res)
        {
            if (res.result && res.result.result == 0)
            {
                self.agocontrol.devices.remove(function(e) {
                    return e.uuid == item.uuid;
                });
            }
            else
            {
                notif.error("Error while deleting scenarios!");
            }
            self.agocontrol.unblock($('#agoGrid'));
        });
    };

    self.editScenario = function(item)
    {
        var content = {};
        content.scenario = item.uuid;
        content.uuid = self.agocontrol.scenarioController;
        content.command = 'getscenario';
        self.agocontrol.sendCommand(content, function(res)
        {
            // Build command list
            for ( var idx in res.result.scenariomap)
            {
                self.addCommand("scenarioBuilderEdit", res.result.scenariomap[idx]);
            }

            // Save the id (needed for the save command)
            self.openScenario = item.uuid;

            // Open the dialog
            if (document.getElementById("editScenarioDialogTitle"))
            {
                $("#editScenarioDialog").dialog({
                    title : document.getElementById("editScenarioDialogTitle").innerHTML,
                    modal : true,
                    width : 940,
                    height : 600,
                    close : function()
                    {
                        // Done, restore stuff
                        document.getElementById("scenarioBuilderEdit").innerHTML = "";
                        self.openScenario = null;
                    }
                });
            }
        });
    };

    self.doEditScenario = function()
    {
        var content = {};
        content.command = "setscenario";
        content.uuid = self.agocontrol.scenarioController;
        content.scenario = self.openScenario;
        content.scenariomap = self.buildScenarioMap("scenarioBuilderEdit");
        self.agocontrol.sendCommand(content, function(res)
        {
            if (res.result && res.result.scenario)
            {
                $("#editScenarioDialog").dialog("close");
            }
        });
    };

    self.runScenario = function(item)
    {
        var content = {};
        content.uuid = item.uuid;
        content.command = 'on';
        self.agocontrol.sendCommand(content);
    };

}

/**
 * Initalizes the model
 */
function init_template(path, params, agocontrol)
{
    var model = new ScenarioConfig(agocontrol);
    return model;
}