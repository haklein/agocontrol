/**
 * Model class
 * 
 * @returns {EventsConfig}
 */
function EventsConfig(agocontrol)
{
    var self = this;
    self.eventName = ko.observable();
    self.agocontrol = agocontrol;
    self.openEvent = null;
    self.current_id = 0;
    self.map = {};
    self.events = ko.computed(function() {
        //get only events
        var events = self.agocontrol.devices().filter(function(d) {
            return d.devicetype=='event';
        });

        //sort devices
        self.agocontrol.devices().sort(function(a, b) {
            return a.name.localeCompare(b.name);
        });

        return events;
    });

    function unblockGrid(){
        self.agocontrol.unblock($('#agoGrid'));
    }

    /**
     * Initalizes the empty event creation builder
     */
    self.initBuilder = function()
    {
        // Clean
        document.getElementsByClassName("eventBuilder")[0].innerHTML = "";
        document.getElementById("actionBuilder").innerHTML = "";

        // Build new
        var eventSelector = self.getEventSelector(self.agocontrol.schema().events, document.getElementsByClassName("eventBuilder")[0]);
        eventSelector.id = "eventSelector";
        document.getElementsByClassName("eventBuilder")[0].appendChild(eventSelector);
        self.addNesting(document.getElementsByClassName("eventBuilder")[0], "and");
        self.createActionBuilder(document.getElementById("actionBuilder"));
    };

    /**
     * Callback for editable table
     */
    self.makeEditable = function(item, td, tr)
    {
        if( $(td).hasClass('edit_event') )
        {
            $(td).editable(
                function(value, settings) {
                    var content = {};
                    content.device = $(this).data('uuid');
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
    };

    self.grid = new ko.agoGrid.viewModel({
        data: self.events,
        columns: [
            {headerText:'Name', rowText:'name'},
            {headerText:'Actions', rowText:''}
        ],
        rowCallback: self.makeEditable,
        rowTemplate: 'rowTemplate'
    });

    //Used for parsing event into JSON structure
    self.getCriteriaIdx = function(str)
    {
        var regex = /.+([0-9]).+/g;
        var matches = regex.exec(str);
        if (!matches)
        {
            return false;
        }
        return matches[1];
    };

    self.parseSubOp = function(str, criteria)
    {
        var data = str.split(/(or|and)/g);
        var sub = [];
        var type = "";
        data = data.filter(function(e) {
            return $.trim(e) != "";
        });

        for ( var i = 0; i < data.length; i++)
        {
            var tmp = data[i];
            if (type == "" && (tmp == "and" || tmp == "or"))
            {
                type = tmp;
                continue;
            }
            var idx = self.getCriteriaIdx(tmp);
            if (idx !== false)
            {
                sub.push(criteria[idx]);
            }
        }

        return {
            "sub" : sub,
            "type" : type == "" ? "and" : type
        };
    };

    //Used for parsing event into JSON structure
    self.parseGroup = function(str, criteria)
    {
        var operation = "";
        var subs = [];
        var parsed = "";

        for ( var i = str.length - 1; i >= 0; i--)
        {
            if (str[i] == "(")
            {
                for ( var j = i + 1; j < str.length; j++)
                {
                    if (str[j] == ")")
                    {
                        break;
                    }
                    operation += str[j];
                }

                if (operation != "" && !operation.match(/^sub/))
                {
                    if (operation.indexOf("sub") != -1)
                    {
                        operation = operation.substring(0, operation.indexOf("sub"));
                    }

                    subs.push(self.parseSubOp(operation, criteria));

                    //In case it is "open ended" we want to leave the operator
                    //for the top level
                    if ($.trim(operation).substr(-3) == "and")
                    {
                        operation = operation.substr(0, operation.length - 5);
                    }
                    if ($.trim(operation).substr(-2) == "or")
                    {
                        operation = operation.substr(0, operation.length - 4);
                    }
                }

                parsed = str.replace(operation, "sub{" + (subs.length - 1) + "}");
                str = str.substr(0, i) + "sub{" + (subs.length - 1) + "}" + str.substr(j + 1);

                //No more unparsed criteria done
                if (parsed.indexOf("criteria") == -1)
                {
                    break;
                }

                if (operation != "")
                {
                    operation = "";
                }
            }
        }

        //Only one level no need for parsing
        if (subs.length == 1)
        {
            return subs[0];
        }

        //Clean up string
        parsed = parsed.replace(/\{/g, "[");
        parsed = parsed.replace(/\}/g, "]");
        parsed = parsed.replace(/\)/g, "");
        parsed = parsed.replace(/\(/g, "");

        //Parse the top level
        var data = parsed.split(/(or|and)/g);

        var sub = [];
        var type = "";
        for( var i = 0; i < data.length; i++)
        {
            var tmp = data[i];
            if (tmp == "and" || tmp == "or")
            {
                type = tmp;
                continue;
            }
            var idx = self.getCriteriaIdx(tmp);
            if (idx !== false)
            {
                sub.push(subs[idx]);
            }
        }

        return {
            "sub" : sub,
            "type" : type
        };
    };

    //Used for parsing event into JSON structure
    self.mapToJSON = function(input)
    {
        var criteria = {};
        for ( var idx in input.criteria)
        {
            criteria[idx] = {};
            criteria[idx].path = input.event;
            criteria[idx].comp = input.criteria[idx].comp;
            criteria[idx].param = input.criteria[idx].lval;
            criteria[idx].value = input.criteria[idx].rval;
        }

        var res = {};
        if (input.nesting == "True")
        {
            res.elements = [];
        }
        else
        {
            res.elements = [ self.parseGroup("(" + input.nesting + ")", criteria) ];
        }
        res.path = input.event;
    
        return res;
    };

    //Opens edit event dialog
    self.editEvent = function(item)
    {
        var content = {};
        content.command = "getevent";
        content.event = item.uuid;
        content.uuid = self.agocontrol.eventController;
        self.agocontrol.sendCommand(content)
            .then(function(res){
                // Swap active selector
                document.getElementById("eventBuilder").className = "";
                document.getElementById("eventBuilderEdit").className = "eventBuilder";

                // Disable main one to avoid id conflicts
                document.getElementById("eventBuilder").innerHTML = "";
                document.getElementById("actionBuilder").innerHTML = "";

                // Create prepopulated builders
                self.buildListFromJSON(self.mapToJSON(res.data.eventmap), document.getElementById("eventBuilderEdit"));
                self.createActionBuilder(document.getElementById("actionBuilderEdit"), res.data.eventmap.action);

                // Save the id (needed for the save command)
                self.openEvent = item.uuid;

                // Open the dialog
                if (document.getElementById("editEventDialogTitle")) {
                    $("#editEventDialog").dialog({
                        title : document.getElementById("editEventDialogTitle").innerHTML,
                        modal : true,
                        width : 900,
                        height : 600,
                        close : function() {
                            // Done, restore stuff
                            document.getElementById("eventBuilderEdit").className = "";
                            document.getElementById("eventBuilder").className = "eventBuilder";
                            self.initBuilder();
                            self.openEvent = null;
                        }
                    });
                }
            })
            .catch(notifCommandError);
    };

    //Sends the event edit command
    self.doEditEvent = function()
    {
        this.createEventMap(self.createJSON());
        var content = {};
        content.uuid = self.agocontrol.eventController;
        content.command = "setevent";
        content.eventmap = self.map;
        content.event = self.openEvent;
        self.agocontrol.sendCommand(content)
            .then(function(res) {
                if (res.data.event)
                {
                    $("#editEventDialog").dialog("close");
                }
            });
    };

    //Sends the create event commands
    self.createEvent = function()
    {
        if( $.trim(self.eventName())=='' )
        {
            notif.warning("Please supply an event name!");
            return;
        }

        self.agocontrol.block($('#agoGrid'));
        this.createEventMap(self.createJSON());
        var content = {};
        content.uuid = self.agocontrol.eventController;
        content.command = "setevent";
        content.eventmap = self.map;
    
        self.agocontrol.sendCommand(content)
            .then(function(res) {
                if (res.data.event)
                {
                    var cnt = {};
                    cnt.uuid = self.agocontrol.agoController;
                    cnt.device = res.data.event;
                    cnt.command = "setdevicename";
                    cnt.name = self.eventName();
                    return self.agocontrol.sendCommand(cnt)
                        .then(function(nameRes) {
                            self.agocontrol.refreshDevices(false);
                            self.initBuilder();
                        });
                }
            })
            .catch(notifCommandError)
            .finally(unblockGrid);
    };

    self.deleteEvent = function(item, event)
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
            self.doDeleteEvent(item, event);
            $("#confirmDelete").dialog("close");
        };
        $("#confirmDelete").dialog({
            modal : true,
            height : 180,
            width : 500,
            buttons : buttons
        });
    };

    //Sends the delete event command
    self.doDeleteEvent = function(item, event)
    {
        self.agocontrol.block($('#agoGrid'));
        var content = {};
        content.event = item.uuid;
        content.uuid = self.agocontrol.eventController;
        content.command = 'delevent';
        self.agocontrol.sendCommand(content)
            .then(function(res) {
                self.agocontrol.devices.remove(function(e) {
                    return e.uuid == item.uuid;
                });
            })
            .catch(notifCommandError)
            .finally(unblockGrid);
    };

    //Helper for event map creation
    self.parseElement = function(element)
    {
        // 'empty' events like 'sun did rise'
        if (element.sub === undefined)
        {
            return "True";
        }

        var nesting = "";

        for ( var j = 0; j < element.sub.length; j++)
        {
            var obj = element.sub[j];
            if (obj.param !== undefined)
            {
                self.map.criteria[self.idx] = {};
                self.map.criteria[self.idx].lval = obj.param;
                self.map.criteria[self.idx].comp = obj.comp;
                self.map.criteria[self.idx].rval = obj.value;
                if (nesting == "")
                {
                    nesting += "(criteria[" + self.idx + "]";
                }
                else
                {
                    nesting += " " + element.type + " criteria[" + self.idx + "]";
                }
                self.idx = self.idx + 1;
            }
            else
            {
                if (nesting == "")
                {
                    nesting += "(";
                }
                else
                {
                    nesting += " " + element.type;
                }

                nesting += " (" + self.parseElement(obj) + ")";
            }
        }

        if (nesting == "")
        {
            return "(True)";
        }

        return nesting + ")";
    };

    //Creates the event map for the resolver
    self.createEventMap = function(data)
    {
        self.map = {};
        self.map.criteria = {};
        self.map.nesting = "";
        self.map.event = data.path;

        self.idx = 0;

        for ( var i = 0; i < data.elements.length; i++)
        {
            self.map.nesting += self.parseElement(data.elements[i]);
            self.idx = self.idx + 1;
        }

        // Add the toplevel operator
        self.map.nesting = self.map.nesting.replace(/\)\(/g, ") " + data.elements[data.elements.length - 1].type + " (");

        // Remove useless and/or suffixes
        self.map.nesting = $.trim(self.map.nesting);
        self.map.nesting = self.map.nesting.replace(/^(and|or)/g, "");
        self.map.nesting = $.trim(self.map.nesting);

        if (self.map.criteria == {})
        {
            self.map.nesting = "True";
        }

        // Set the action
        self.map.action = {};
        self.map.action.command = document.getElementById("commandSelect").options[document.getElementById("commandSelect").selectedIndex].value;
        self.map.action.uuid = self.agocontrol.devices()[document.getElementById("deviceListSelect").options[document.getElementById("deviceListSelect").selectedIndex].value].uuid;

        var paramList = document.getElementsByClassName("cmdParam");
        if (paramList)
        {
            for ( var i = 0; i < paramList.length; i++)
            {
                self.map.action[paramList[i].name] = paramList[i].value;
            }
        }
    };

    //Adds a nested element
    self.addNesting = function(container, type)
    {
        var dl = document.createElement("dl");
        var dd = document.createElement("dd");
        dl.setAttribute("class", "operator");

        var subList = document.createElement("dl");
        subList.setAttribute("class", "subList");

        dl.appendChild(dd);
        dd.appendChild(document.createTextNode("Operator: "));

        var radio = document.createElement("input");
        radio.name = "op_" + (self.current_id);
        radio.id = radio.name + "_and";
        radio.type = "radio";
        radio.value = "and";
        radio.checked = (type == "and");
        var label = document.createElement("label");
        label.appendChild(document.createTextNode("and"));
        label.setAttribute("for", radio.id);
        dd.appendChild(radio);
        dd.appendChild(label);

        var radio = document.createElement("input");
        radio.name = "op_" + (self.current_id);
        radio.id = radio.name + "_or";
        radio.type = "radio";
        radio.value = "or";
        radio.checked = (type == "or");
        var label = document.createElement("label");
        label.appendChild(document.createTextNode("or"));
        label.setAttribute("for", radio.name);
        dd.appendChild(radio);
        dd.appendChild(label);

        self.current_id++;

        var adSbutton = document.createElement("button");
        adSbutton.appendChild(document.createTextNode("Add criteria"));
        adSbutton._list = subList;
        adSbutton.onclick = function()
        {
            self.addSegment(this._list);
        };

        dd.appendChild(adSbutton);

        var adNbutton = document.createElement("button");
        adNbutton.appendChild(document.createTextNode("Add nesting"));
        adNbutton._list = dl;
        adNbutton.onclick = function()
        {
            self.addNesting(this._list.lastChild, "and");
        };

        dd.appendChild(adNbutton);

        var button = document.createElement("button");
        button.appendChild(document.createTextNode("delete"));
        button._list = dl;
        button.onclick = function()
        {
            if (this._list.parentNode == document.getElementsByClassName("eventBuilder")[0] && document.getElementsByClassName("operator").length == 1)
            {
                notif.warning("Last element cannot be deleted!");
                return;
            }
            this._list.parentNode.removeChild(this._list);
        };

        dd.appendChild(button);

        var button = document.createElement("button");
        button.appendChild(document.createTextNode("<"));
        button._list = dl;
        button.onclick = function()
        {
            if (this._list.parentNode == document.getElementsByClassName("eventBuilder")[0])
            {
                notif.warning("This is already a top level element!");
                return;
            }
            var parentElem = dl.parentNode;
            var newParent = dl.parentNode.parentNode.parentNode;
            parentElem.removeChild(dl);
            if (parentElem.nextSibling)
            {
                newParent.insertBefore(dl, parentElem.nextSibling);
            }
            else
            {
                newParent.appendChild(dl);
            }
        };

        dd.appendChild(button);
    
        var button = document.createElement("button");
        button.appendChild(document.createTextNode(">"));
        button._list = dl;
        button.onclick = function()
        {
            var ops = document.getElementsByClassName("operator");
            if (ops.length == 1)
            {
                notif.warning("This is already a top level element!");
                return;
            }
            else
            {
                var parentElem = dl.parentNode;
                var newParent = dl;
                while ((newParent = newParent.previousSibling) != null)
                {
                    if (newParent.nodeType == 1)
                    {
                        break;
                    }
                }

                if (newParent.className != "operator")
                {
                    notif.warning("Item is already at the maximum indent level!");
                    return;
                }
                parentElem.removeChild(dl);
                newParent.lastChild.appendChild(dl);
            }
        };

        dd.appendChild(button);

        var tmp = document.createElement("dd");
        tmp.appendChild(subList);
        dl.appendChild(tmp);

        container.appendChild(dl);

        return [ dl, subList ];
    };

    //Adds a new segment
    self.addSegment = function(container, eventObj)
    {
        var dd = document.createElement("dd");

        var span = document.createElement("span");
        span.className = "eventParams";
    
        dd.appendChild(span);
    
        if (eventObj)
        {
            self.renderEvent(eventObj.param.type, eventObj.path, self.agocontrol.schema().events[eventObj.path], span, eventObj);
        }
        else
        {
            var selector = document.getElementById("eventSelector");
            self.renderEvent("event", selector.options[selector.selectedIndex].value, self.agocontrol.schema().events[selector.options[selector.selectedIndex].value], span);
        }

        var button = document.createElement("button");
        button.appendChild(document.createTextNode("delete"));
        button._listItem = dd;
        button.onclick = function()
        {
            this._listItem.parentNode.removeChild(this._listItem);
        };
        dd.appendChild(button);
        container.appendChild(dd);
    };

    //Converts one operation to JSON
    self.operationToJSON = function(tmp)
    {
        var op = {};
        op.sub = [];
        var radios = tmp.firstChild.getElementsByTagName("input");
        for ( var j = 0; j < radios.length; j++)
        {
            if (radios[j].checked)
            {
                op.type = radios[j].value;
            }
        }

        if (tmp.getElementsByClassName("subList").length > 0)
        {
            var subList = tmp.getElementsByClassName("subList")[0];
            for ( var j = 0; j < subList.childNodes.length; j++)
            {
                var eventObj = {};
                var selects = subList.childNodes[j].getElementsByTagName("select");
                var selector = document.getElementById("eventSelector");
                eventObj.path = selector.options[selector.selectedIndex].value;

                if (selects.length > 0)
                {
                    var type = selects[0].options[selects[0].selectedIndex].value;
                    if (type == "event")
                    {
                        eventObj.comp = selects[2].options[selects[2].selectedIndex].value;

                        eventObj.param = {};
                        eventObj.param.type = "event";
                        eventObj.param.parameter = selects[1].options[selects[1].selectedIndex].value;
    
                        eventObj.value = subList.childNodes[j].getElementsByTagName("input")[0].value;
                    }
                    else if (type == "device")
                    {
                        eventObj.comp = selects[3].options[selects[3].selectedIndex].value;

                        eventObj.param = {};
                        eventObj.param.type = "device";
                        eventObj.param.uuid = selects[1].options[selects[1].selectedIndex].value;
                        eventObj.param.parameter = selects[2].options[selects[2].selectedIndex].value;

                        eventObj.value = subList.childNodes[j].getElementsByTagName("input")[0].value;
                    }
                    if (type == "variable")
                    {
                        eventObj.comp = selects[2].options[selects[2].selectedIndex].value;
                        eventObj.param = {};
                        eventObj.param.type = "variable";
                        eventObj.param.name = selects[1].options[selects[1].selectedIndex].value;
                        eventObj.value = subList.childNodes[j].getElementsByTagName("input")[0].value;
                    }
                }
                op.sub.push(eventObj);
            }

            var subOps = subList.parentNode.childNodes;
            for ( var j = 0; j < subOps.length; j++)
            {
                if (subOps[j].className == "operator")
                {
                    op.sub.push(self.operationToJSON(subOps[j]));
                }
            }
        }

        return op;
    };

    //Create a JSON structure out of the whole event
    self.createJSON = function()
    {
        var ops = document.getElementsByClassName("operator");
        var res = [];
        for ( var i = 0; i < ops.length; i++)
        {
            if (ops[i].parentNode != document.getElementsByClassName("eventBuilder")[0])
            {
                continue;
            }
            res.push(self.operationToJSON(ops[i]));
        }

        var eventSelector = document.getElementById("eventSelector");
        res = {
            "elements" : res,
            "path" : eventSelector.options[eventSelector.selectedIndex].value
        };

        return res;
    };

    //Creates a new operation
    self.createOperation = function(sub, container, type, toplevel)
    {
        if (toplevel)
        {
            self.createOperation(sub[0].sub, container, sub[0].type);
            return;
        }
        var res = self.addNesting(container, type);
        var dl = res[0];
        var subList = res[1];
        for ( var i = 0; i < sub.length; i++)
        {
            if (sub[i].type)
            {
                self.createOperation(sub[i].sub, dl.lastChild, sub[i].type);
            }
            else
            {
                self.addSegment(subList, sub[i]);
            }
        }
    };

    //(re) Builds the list from JSON
    self.buildListFromJSON = function(input, container)
    {
        document.getElementsByClassName("eventBuilder")[0].innerHTML = "";
        var eventSelector = self.getEventSelector(self.agocontrol.schema().events, document.getElementsByClassName("eventBuilder")[0]);
        eventSelector.id = "eventSelector";
        document.getElementsByClassName("eventBuilder")[0].appendChild(eventSelector);

        var inputList = input.elements;
        inputList.reverse();
        for ( var i = 0; i < inputList.length; i++)
        {
            var op = inputList[i];
            self.createOperation(op.sub, document.getElementsByClassName("eventBuilder")[0], op.type);
        }

        var eventSelector = document.getElementById("eventSelector");
        for ( var i = 0; i < eventSelector.options.length; i++)
        {
            if (eventSelector.options[i].value == input.path)
            {
                eventSelector.options[i].selected = true;
                break;
            }
        }
    };

    //Creates the event selector
    self.getEventSelector = function(events, container, defaultPath)
    {
        var eventList = document.createElement("select");
        eventList.onchange = function()
        {
            var path = eventList.options[eventList.selectedIndex].value;
            var eventParams = document.getElementsByClassName("eventParams");
            for ( var i = 0; i < eventParams.length; i++)
            {
                self.renderEvent("event", path, events[path], eventParams[i]);
            }
        };
        var i = 0;
        for (path in events)
        {
            var opt = new Option(events[path].description, path);
            eventList.options[i] = opt;
            if (defaultPath && defaultPath == path)
            {
                eventList.options[i].selected = true;
            }
            i++;
        }

        return eventList;
    };

    //Renders an event
    self.renderEvent = function(selectType, path, event, container, defaultValues)
    {
        container.innerHTML = "";

        var params = null;

        var baseType = document.createElement("select");
        baseType.options[0] = new Option("event", "event");
        baseType.options[1] = new Option("device", "device");
        baseType.options[2] = new Option("variable", "variable");
        container.appendChild(baseType);

        if (selectType == "event")
        {
            baseType.selectedIndex = 0;
        }
        else if (selectType == "device")
        {
            baseType.selectedIndex = 1;
        }
        else if (selectType == "variable")
        {
            baseType.selectedIndex = 2;
        }

        baseType.onchange = function()
        {
            var type = baseType.options[baseType.selectedIndex].value;
            self.renderEvent(type, path, event, container, defaultValues);
        };

        if (selectType == "event")
        {
            params = document.createElement("select");
            params.name = path + ".param";
            if (event.parameters !== undefined)
            {
                for ( var i = 0; i < event.parameters.length; i++)
                {
                    var opt = null;
                    if (event.parameters[i] == "uuid")
                    {
                        opt = new Option("device", event.parameters[i]);
                    }
                    else
                    {
                        opt = new Option(event.parameters[i], event.parameters[i]);
                    }
                    params.options[i] = opt;
                    if (defaultValues && event.parameters[i] == defaultValues.param.parameter)
                    {
                        params.options[i].selected = true;
                    }
                }
            }

            container.appendChild(params);
        }
        else if (selectType == "device")
        {
            var deviceSelect = document.createElement("select");
            deviceSelect.name = path + ".device";
            /*self.agocontrol.devices().sort(function(a, b)
            {
                return a.room.localeCompare(b.room);
            });*/
            for ( var i = 0; i < self.agocontrol.devices().length; i++)
            {
                if (self.agocontrol.devices()[i].name)
                {
                    var dspName = "";
                    if (self.agocontrol.devices()[i].room)
                    {
                        dspName = self.agocontrol.devices()[i].room + " - " + self.agocontrol.devices()[i].name;
                    }
                    else
                    {
                        dspName = self.agocontrol.devices()[i].name;
                    }
                    deviceSelect.options[deviceSelect.options.length] = new Option(dspName, self.agocontrol.devices()[i].uuid);
                    if (defaultValues && self.agocontrol.devices()[i].uuid == defaultValues.param.uuid)
                    {
                        deviceSelect.options[deviceSelect.options.length - 1].selected = true;
                    }
                }
            }

            params = document.createElement("select");
            params.name = path + ".param";

            var _buildParamList = function(dev)
            {
                params.options.length = 0;
                params.options[0] = new Option("state", "state");
                if (dev.values)
                {
                    var i = 0;
                    for ( var k in dev.values())
                    {
                        params.options[i + 1] = new Option(k, k);
                        if (defaultValues && k == defaultValues.param.parameter)
                        {
                            params.options[i + 1].selected = true;
                        }
                        i++;
                    }
                }
            };

            _buildParamList(self.agocontrol.devices()[deviceSelect.options.selectedIndex]);

            deviceSelect.onchange = function()
            {
                var selectedUuid = deviceSelect.options[deviceSelect.selectedIndex].value;
                var dev = {};
                for ( var i = 0; i < self.agocontrol.devices().length; i++)
                {
                    if (self.agocontrol.devices()[i].uuid == selectedUuid)
                    {
                        dev = self.agocontrol.devices()[i];
                        break;
                    }
                }
                _buildParamList(dev);
            };

            container.appendChild(deviceSelect);
            container.appendChild(params);
            deviceSelect.onchange();
        }
        else if (selectType == "variable")
        {
            params = document.createElement("select");
            params.name = path + ".param";
            var i = 0;
            for ( var k in variables)
            {
                params.options[i] = new Option(k, k);
                if (defaultValues && k == defaultValues.param.name)
                {
                    params.options[i].selected = true;
                }
                i++;
            }
            container.appendChild(params);
        }

        var comp = document.createElement("select");
        comp.name = path + ".comp";
        comp.options[0] = new Option("=", "eq");
        comp.options[1] = new Option("!=", "new");
        comp.options[2] = new Option(">", "gt");
        comp.options[3] = new Option("<", "lt");
        comp.options[4] = new Option(">=", "gte");
        comp.options[5] = new Option("<=", "lte");

        if (defaultValues)
        {
            for ( var i = 0; comp.options.length; i++)
            {
                if (comp.options[i].value == defaultValues.comp)
                {
                    comp.options[i].selected = true;
                    break;
                }
            }
        }

        var inputField = document.createElement("input");
        inputField.name = path + ".value";
        if (defaultValues)
        {
            inputField.value = defaultValues.value;
        }

        /* Show variable value placeholder */
        if (selectType == "variable")
        {
            inputField.setAttribute("placeholder", variables[params.options[0].value]);
            params.onchange = function()
            {
                inputField.setAttribute("placeholder", variables[params.options[params.selectedIndex].value]);
            };
        }

        container.appendChild(comp);
        container.appendChild(inputField);

        /* Add a device selector when someone selects the uuid field of event */
        if (selectType == "event")
        {
            var deviceSelect = document.createElement("select");
            deviceSelect.name = path + ".device";
            for ( var i = 0; i < self.agocontrol.devices().length; i++)
            {
                if (self.agocontrol.devices()[i].name)
                {
                    var dspName = "";
                    if (self.agocontrol.devices()[i].room)
                    {
                        dspName = self.agocontrol.devices()[i].room + " - " + self.agocontrol.devices()[i].name;
                    }
                    else
                    {
                        dspName = self.agocontrol.devices()[i].name;
                    }
                    deviceSelect.options[deviceSelect.options.length] = new Option(dspName, self.agocontrol.devices()[i].uuid);
                    if (defaultValues && defaultValues.param.parameter == "uuid" && self.agocontrol.devices()[i].uuid == defaultValues.value)
                    {
                        deviceSelect.selectedIndex = deviceSelect.options.length - 1;
                    }
                }
            }
            deviceSelect.onchange = function()
            {
                inputField.value = deviceSelect.options[deviceSelect.selectedIndex].value;
            };
            deviceSelect.style.display = "none";
            container.appendChild(deviceSelect);
            params.onchange = function()
            {
                if (params.options[params.selectedIndex].value == "uuid")
                {
                    deviceSelect.style.display = "";
                    inputField.style.display = "none";
                }
                else
                {
                    deviceSelect.style.display = "none";
                    inputField.style.display = "";
                    inputField.value = "";
                }
            };
            if (defaultValues && defaultValues.param.parameter == "uuid")
            {
                params.onchange();
            }
        }
    };

    //Creates the action builder
    self.createActionBuilder = function(container, defaults)
    {
        container.innerHTML = "";
        var deviceListSelect = document.createElement("select");
        deviceListSelect.id = "deviceListSelect";
        var commandSelect = document.createElement("select");
        commandSelect.id = "commandSelect";
        var commandParams = document.createElement("fieldset");
        var j = 0;
        for ( var i = 0; i < self.agocontrol.devices().length; i++)
        {
            var dev = self.agocontrol.devices()[i];
            if (self.agocontrol.schema().devicetypes[dev.devicetype] === undefined || self.agocontrol.schema().devicetypes[dev.devicetype].commands.length == 0)
            {
                continue;
            }
            if (self.agocontrol.devices()[i].name)
            {
                var dspName = "";
                if (self.agocontrol.devices()[i].room)
                {
                    dspName = self.agocontrol.devices()[i].room + " - " + self.agocontrol.devices()[i].name;
                }
                else
                {
                    dspName = self.agocontrol.devices()[i].name;
                }
                deviceListSelect.options[j] = new Option(dspName, i);
                if (defaults && defaults.uuid == dev["uuid"])
                {
                    deviceListSelect.options[j].selected = true;
                }
                j++;
            }
        }

        container.appendChild(deviceListSelect);
        container.appendChild(commandSelect);
        container.appendChild(commandParams);
    
        deviceListSelect.onchange = function()
        {
            if (deviceListSelect.options[deviceListSelect.selectedIndex] == undefined)
            {
                return;
            }
            var idx = deviceListSelect.options[deviceListSelect.selectedIndex].value;
            self.createCommandSelector(commandSelect, commandParams, self.agocontrol.devices()[idx].devicetype);
        };
        if (!defaults)
        {
            deviceListSelect.onchange();
        }
        else
        {
            var idx = deviceListSelect.options[deviceListSelect.selectedIndex].value;
            self.createCommandSelector(commandSelect, commandParams, self.agocontrol.devices()[idx].devicetype, defaults);
        }
    };

    //Creates the command selector
    self.createCommandSelector = function(commandSelect, commandParams, type, defaults)
    {
        commandSelect.options.length = 0;
        for ( var i = 0; i < self.agocontrol.schema().devicetypes[type].commands.length; i++)
        {
            commandSelect.options[i] = new Option(self.agocontrol.schema().commands[self.agocontrol.schema().devicetypes[type].commands[i]].name, self.agocontrol.schema().devicetypes[type].commands[i]);
            if (defaults && defaults.command == self.agocontrol.schema().devicetypes[type].commands[i])
            {
                commandSelect.options[i].selected = true;
            }
        }

        commandSelect.onchange = function()
        {
            var cmd = self.agocontrol.schema().commands[commandSelect.options[commandSelect.selectedIndex].value];
            commandParams.innerHTML = "";
            if (cmd.parameters !== undefined)
            {
                commandParams.style.display = "";
                var legend = document.createElement("legend");
                legend.style.fontWeight = 700;
                legend.appendChild(document.createTextNode("Parameters"));
                commandParams.appendChild(legend);
                for ( var param in cmd.parameters)
                {
                    var label = document.createElement("label");
                    label.appendChild(document.createTextNode(cmd.parameters[param].name + ": "));
                    label.setAttribute("for", cmd.parameters[param].name);
                    commandParams.appendChild(label);

                    if (cmd.parameters[param].type == 'option')
                    {
                        var select = document.createElement("select");
                        select.name = param;
                        select.className = "cmdParam";
                        select.id = cmd.parameters[param].name;
                        for ( var i = 0; i < cmd.parameters[param].options.length; i++)
                        {
                            select.options[select.options.length] = new Option(cmd.parameters[param].options[i], cmd.parameters[param].options[i]);
                        }
                    commandParams.appendChild(select);
                    }
                    else
                    {
                        var input = document.createElement("input");
                        input.name = param;
                        input.id = cmd.parameters[param].name;
                        input.className = "cmdParam";
                        if (defaults && defaults[param])
                        {
                            input.value = defaults[param];
                        }
                        commandParams.appendChild(input);
                    }

                    var br = document.createElement("br");
                    commandParams.appendChild(br);
                }
            }
            else
            {
                commandParams.style.display = "none";
            }
        };
    
        commandSelect.onchange();
    };

}

/**
 * Initalizes the model
 */
function init_template(path, params, agocontrol)
{
    var model = new EventsConfig(agocontrol);
    return model;
}

function afterrender_template(model)
{
    //template is rendered, we can build graph
    if( model )
    {
        model.initBuilder();
    }
}

