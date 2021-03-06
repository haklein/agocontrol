/**
 * Model class
 * 
 * @returns {DeviceConfig}
 */
function DeviceConfig(agocontrol)
{
    var self = this;
    self.agocontrol = agocontrol;
    self.roomFilters = ko.observableArray([]);
    self.deviceTypeFilters = ko.observableArray([]);
    self.handlerFilters = ko.observableArray([]);

    self.updateRoomFilters = function() {
        var tagMap = {};
        for ( var i = 0; i < self.agocontrol.devices().length; i++)
        {
            var dev = self.agocontrol.devices()[i];
            if (dev.roomUID)
            {
                if( !tagMap["room_" + dev.room] )
                {
                    tagMap["room_" + dev.room] = {
                        column : "room",
                        value : dev.room,
                        selected : false,
                        className : "btn btn-default btn-xs"
                    };
                }
            }
        }

        var roomList = [];
        for ( var k in tagMap)
        {
            var entry = tagMap[k];
            if (entry.column != "devicetype")
            {
                roomList.push(entry);
            }
        }

        roomList.sort(function(a, b) {
            if( a.value<b.value ) return -1;
            else if( a.value>b.value ) return 1;
            else return 0;
        });

        self.roomFilters(roomList);
    };

    self.updateDeviceTypeFilters = function() {
        var tagMap = {};
        for ( var i = 0; i < self.agocontrol.devices().length; i++)
        {
            var dev = self.agocontrol.devices()[i];
            if( !tagMap["type_" + dev.devicetype] )
            {
                tagMap["type_" + dev.devicetype] = {
                    column : "devicetype",
                    value : dev.devicetype,
                    selected : false,
                    className : "btn btn-default btn-xs"
                };
            }
        }

        var devList = [];
        for ( var k in tagMap)
        {
            var entry = tagMap[k];
            if (entry.column == "devicetype")
            {
                devList.push(entry);
            }
        }

        devList.sort(function(a, b) {
            if( a.value<b.value ) return -1;
            else if( a.value>b.value ) return 1;
            else return 0;
        });

        self.deviceTypeFilters(devList);
    };

    self.updateHandlerFilters = function() {
        var tagMap = {};
        for( var i=0; i<self.agocontrol.devices().length; i++ )
        {
            var dev = self.agocontrol.devices()[i];
            if( !tagMap["handler_" + dev.handledBy] )
            {
                tagMap["handler_" + dev.handledBy] = {
                        column : "handledBy",
                        value : dev.handledBy,
                        selected : false,
                        className : "btn btn-default btn-xs"
                };
            }
        }

        var handlerList = [];
        for( var k in tagMap )
        {
            var entry = tagMap[k];
            if( entry.column=="handledBy" )
            {
                handlerList.push(entry);
            }
        }

        handlerList.sort(function(a,b) {
            if( a.value<b.value ) return -1;
            else if( a.value>b.value ) return 1;
            else return 0;
        });

        self.handlerFilters(handlerList);
    }

    self.addFilter = function(item) {
        var tmp = "";
        var i = 0;
        if (item.column == "devicetype")
        {
            //update selected devicetype filters
            for ( i = 0; i < self.deviceTypeFilters().length; i++)
            {
                if (self.deviceTypeFilters()[i].value == item.value)
                {
                    if( item.className == "btn btn-default btn-xs" )
                    {
                        self.deviceTypeFilters()[i].selected = true;
                        self.deviceTypeFilters()[i].className = "btn btn-primary btn-xs";
                    }
                    else
                    {
                        self.deviceTypeFilters()[i].selected = false;
                        self.deviceTypeFilters()[i].className = "btn btn-default btn-xs";
                    }
                }
            }
            tmp = self.deviceTypeFilters();
            self.deviceTypeFilters([]);
            self.deviceTypeFilters(tmp);
        }
        else if( item.column=="room" )
        {
            //update selected room filters
            for ( i = 0; i < self.roomFilters().length; i++)
            {
                if (self.roomFilters()[i].value == item.value)
                {
                    if( item.className == "btn btn-default btn-xs" )
                    {
                        self.roomFilters()[i].selected = true;
                        self.roomFilters()[i].className = "btn btn-primary btn-xs";
                    }
                    else
                    {
                        self.roomFilters()[i].selected = false;
                        self.roomFilters()[i].className = "btn btn-default btn-xs";
                    }
                }
            }
            tmp = self.roomFilters();
            self.roomFilters([]);
            self.roomFilters(tmp);
        }
        else if( item.column=="handledBy" )
        {
            //update selected handler filters
            for( i=0; i<self.handlerFilters().length; i++ )
            {
                if( self.handlerFilters()[i].value==item.value )
                {
                    if( item.className=="btn btn-default btn-xs" )
                    {
                        self.handlerFilters()[i].selected = true;
                        self.handlerFilters()[i].className = "btn btn-primary btn-xs";
                    }
                    else
                    {
                        self.handlerFilters()[i].selected = false;
                        self.handlerFilters()[i].className = "btn btn-default btn-xs";
                    }
                }
            }
            tmp = self.handlerFilters();
            self.handlerFilters([]);
            self.handlerFilters(tmp);
        }

        //apply filters to grid
        self.grid.resetFilters();
        for( var i=0; i<self.roomFilters().length; i++ )
        {
            if( self.roomFilters()[i].selected )
            {
                self.grid.addFilter('room', self.roomFilters()[i].value);
            }
        }
        for( var i=0; i<self.deviceTypeFilters().length; i++ )
        {
            if( self.deviceTypeFilters()[i].selected )
            {
                self.grid.addFilter('devicetype', self.deviceTypeFilters()[i].value);
            }
        }
        for( var i=0; i<self.handlerFilters().length; i++ )
        {
            if( self.handlerFilters()[i].selected )
            {
                self.grid.addFilter('handledBy', self.handlerFilters()[i].value);
            }
        }
    };

    self.makeEditable = function(item, td, tr) {
        // rowCallback, called when a cell is clicked
        if( $(td).hasClass('edit_device') ) {
            self.agocontrol.makeFieldDeviceNameEditable(td, item);
        }
        if( $(td).hasClass('select_device_room') ) {
            self.agocontrol.makeFieldDeviceRoomEditable(td, item,
                {
                    callback: function(){
                        //update room filters
                        self.updateRoomFilters();
                    }
                });
        }
    };

    self.grid = new ko.agoGrid.viewModel({
        data: self.agocontrol.devices,
        pageSize: 25,
        columns: [
            {headerText:'Name', rowText:'name'},
            {headerText:'Room', rowText:'room'},
            {headerText:'Device type', rowText:'devicetype'},
            {headerText:'Handled by', rowText:'handledBy'},
            {headerText:'Internalid', rowText:'internalid'},
            {headerText:'Actions', rowText:''},
            {headerText:'', rowText:'uuid'}
        ],
        rowCallback: self.makeEditable,
        rowTemplate: 'rowTemplate',
        boxStyle: 'box-primary'
    });

    self.deleteDevice = function(item, event)
    {
        $("#confirmPopup").data('item', item);
        $("#confirmPopup").modal('show');
    };

    self.doDeleteDevice = function()
    {
        self.agocontrol.block($('#agoGrid'));
        $("#confirmPopup").modal('hide');

        var item = $("#confirmPopup").data('item');
        var content = {};
        content.device = item.uuid;
        content.uuid = self.agocontrol.agoController;
        content.command = "deletedevice";
        self.agocontrol.sendCommand(content)
            .then(function(res) {
                self.agocontrol.devices.remove(function(e) {
                    return e.uuid == item.uuid;
                });
            })
            .finally(function() {
                self.agocontrol.unblock($('#agoGrid'));
            });
    };

    //update filters
    self.updateRoomFilters();
    self.updateDeviceTypeFilters();
    self.updateHandlerFilters();
}

/**
 * Initalizes the model
 */
function init_template(path, params, agocontrol)
{
    var model = new DeviceConfig(agocontrol);
    return model;
}

