//Agocontrol object
function Agocontrol()
{
    this._init();
};

Agocontrol.prototype = {
    //private members
    subscription: null,
    url: 'jsonrpc',
    multigraphThumbs: [],
    deferredMultigraphThumbs: [],
    refreshMultigraphThumbsInterval: null,
    eventHandlers: [],
    deviceSize: null,
    _allApplications: ko.observableArray([]),
    _allProtocols: ko.observableArray([]),
    _getApplications: Promise.pending(),
    _getProtocols: Promise.pending(),
    _favorites: ko.observable(),
    _noProcesses: ko.observable(false),

    //public members
    devices: ko.observableArray([]),
    environment: ko.observableArray([]),
    rooms: ko.observableArray([]),
    schema: ko.observable(),
    system: ko.observable(),
    variables: ko.observableArray([]),
    supported_devices: ko.observableArray([]),
    processes: ko.observableArray([]),
    applications: ko.observableArray([]),
    protocols: ko.observableArray([]),
    dashboards: ko.observableArray([]),
    configurations: ko.observableArray([]),
    helps: ko.observableArray([]),
    serverTime: 0,
    serverTimeUi: ko.observable(''),
    journalEntries: ko.observableArray([]),
    journalStatus: ko.observable('label label-success'),
    intervalJournal: null,

    agoController: null,
    scenarioController: null,
    eventController: null,
    inventory: null,
    dataLoggerController: null,
    systemController: null,
    journal: null,

    //ui config
    skin: ko.observable('skin-yellow-light body-light'),
    theme: null,
    darkStyle: ko.observable(false),

    _init : function(){
        /**
         * Update application list when we have raw list of application, favorites
         * and processes list
         */
        ko.computed(function(){
            var allApplications = this._allApplications();
            var allProtocols = this._allProtocols();
            var favorites = this._favorites();
            var processes = this.processes();
            // Hack to get around lack of process list on FreeBSD
            var noProcesses = this._noProcesses();

            if(!allApplications.length || !favorites || (!noProcesses && !processes.length))
            {
                //console.log("Not all data ready, trying later");
                return;
            }

            var applications = [];
            //always display "application list" app on top of list
            for( var i=0; i<allApplications.length; i++ )
            {
                var application = allApplications[i];
                if( application.name=='Application list' )
                {
                    application.favorite = true; //applications always displayed
                    application.fav = ko.observable(application.favorite);
                    applications.push(application);
                    break;
                }
            }

            //add all other applications
            for( var i=0; i<allApplications.length; i++ )
            {
                var application = allApplications[i];
                var append = false;

                if( application.name!='Application list' )
                {
                    application.favorite = !!favorites[application.dir];
                    if( application.depends===undefined ||
                            (application.depends!==undefined && $.trim(application.depends).length==0) )
                    {
                        append = true;
                    }
                    else
                    {
                        //check if process is installed (and not if it's currently running!)
                        var proc = this.findProcess(application.depends);
                        if( proc )
                        {
                            append = true;
                        }
                    }
                }

                if(append)
                {
                    application.fav = ko.observable(application.favorite);
                    applications.push(application);
                }
            }

            this.applications(applications);
            this._getApplications.resolve();

            //protocols
            var protocols = [];
            for( var i=0; i<allProtocols.length; i++ )
            {
                var protocol = allProtocols[i];
                var append = false;

                if( protocol.depends===undefined || (protocol.depends!==undefined && $.trim(protocol.depends).length==0) )
                {
                    append = true;
                }
                else
                {
                    //check if process is installed (and not if it's currently running!)
                    var proc = this.findProcess(protocol.depends);
                    if( proc )
                    {
                        append = true;
                    }
                }

                if( append )
                {
                    protocols.push(protocol);
                }
            }

            this.protocols(protocols);
            this._getProtocols.resolve();

        }, this);


    },

    /**
     * Main entrypoint for application.
     * Fetches inventory and other important data. The returned deferred
     * is resolved when basic stuff such as inventory, applications, help pages
     * etc have been loaded (getInventory + updateListing).
     *
     * Application availability is NOT guaranteed to be loaded immediately.
     */
    initialize : function()
    {
        var self = this;

        //get device size
        self.deviceSize = getDeviceSize();

        //get ui config (localstorage)
        //this is used to avoid skin refresh at startup
        if( typeof(Storage)!=="undefined" )
        {
            var skin = localStorage.getItem("skin");
            if( skin )
            {
                self.theme = self.getThemeFromSkin(skin);
                if( skin.indexOf('light')===-1 )
                {
                    self.darkStyle(true);
                }
                else
                {
                    self.darkStyle(false);
                }
                self.skin(skin);
            }
        }

        //handle dark/light style changes
        self.darkStyle.subscribe(function(value) {
            if( self.theme )
            {
                self.setSkin(self.theme);
            }
        });

        var p0 = this.getUiConfig();
        var p1 = this.getInventory()
            .then(this.handleInventory.bind(this))
            .then(this.getJournalEntries.bind(this)); //need datalogger uuid
        var p2 = this.updateListing();

        // Required but non-dependent
        this.updateFavorites();

        return Promise.all([p0, p1, p2]);
    },

    /**
     * Send a command to an arbitrary Ago component.
     *
     * (if oldstyleCallback is ommited and a numeric is passed as second parameter,
     * we will use that as timeout)
     *
     * @param content Should be a dict with any parameters to broadcast to the qpid bus.
     * @param oldstyleCallback A deprecated callback; do not use for new code!
     * @param timeout How many seconds we want the RPC gateway to wait for response
     *
     * @return A promise which will be either resolved or rejected
     */
    sendCommand: function(content, oldstyleCallback, timeout)
    {
        if(oldstyleCallback !== undefined && timeout === undefined &&
                $.isNumeric(oldstyleCallback))
        {
            // Support sendCommand(content, timeout)
            timeout = oldstyleCallback;
            oldstyleCallback = null;
        }

        var self = this;
        var request = {};
        request.jsonrpc = "2.0";
        request.id = 1;
        request.method = "message";
        request.params = {};
        request.params.content = content;

        if (timeout)
        {
            request.params.replytimeout = timeout;
        }

        var promise = new Promise(function(resolve, reject){
            $.ajax({
                    type : 'POST',
                    url : self.url,
                    data : JSON.stringify(request),
                    dataType : "json",
                    success: function(r, textStatus, jqXHR) {
                        // JSON-RPC call gave JSON-RPC response

                        // Old-style callback users
                        if (oldstyleCallback)
                        {
                            // deep copy; we do not want to modify the response sent to new style
                            // handlers
                            var old = {};
                            $.extend(true, old, r);
                            if(old._temp_newstyle_response)
                            {
                                // New-style backend, but old-style UI code.
                                // Add some magic to fool the not-yet-updated code which is
                                // checking for returncode

                                if(old.result && !old.result.returncode)
                                {
                                    old.result.returncode = "0";
                                }
                                else if(old.error)
                                {
                                    if(!old.result)
                                        old.result = {};

                                    old.result.returncode = "-1";
                                }

                                delete old._temp_newstyle_response;
                            }
                            else if(old.error && old.error.identifier === 'error.no.reply')
                            {
                                // Previously agorpc added a result no-reply rather
                                // than using an error..
                                old.result = 'no-reply';
                            }

                            oldstyleCallback(old);
                        }

                        // New-style code should use promise pattern instead,
                        // which is either resolved or rejected with result OR error
                        if(r.result)
                        {
                            if( r.result.message && $.trim(r.result.message).length>0 )
                            {
                                //message specified, display it
                                notif.success(r.result.message);
                            }
                            resolve(r.result);
                        }
                        else
                        {
                            reject(r.error);
                        }
                    },
                    error: function(jqXHR, textStatus, errorThrown)
                    {
                        // Failed to talk properly with agorpc
                        console.error('sendCommand failed for:');
                        console.error(content);
                        console.error('with errors:');
                        console.error(arguments);

                        // old: oldstyleCallback was never called on errors.

                        // Simulate JSON-RPC error
                        reject({
                                code:-32603,
                                message: 'transport.error',
                                data:{
                                    message:'Failed to talk with agorpc'
                                }
                            });
                    }
                });// $.ajax()
            }); // Promise()

        /* Attach a general .catch which logs all messages to notif
         * Note that the application code can still add any .catch handlers
         * on the returned promise, if required.
         */
        promise.catch(function(error){
            if( error.message && error.message==='no.reply' )
            {
                //controller is not responding
                notif.fatal('Controller is not responding');
            }
            else if( error.data && error.data.message )
            {
                notif.error(error.data.message);
            }
            else if( error.message && error.message )
            {
                notif.error(error.message);
            }
            else
            {
                notif.error("Failed: TODO improve: " + JSON.stringify(error));
            }
        });

        return promise;
    },

    //find room
    findRoom: function(uuid)
    {
        var self = this;

        var rooms = self.rooms();
        for( var i=0; i<rooms.length; i++ )
        {
            if( rooms[i].uuid==uuid )
            {
                return rooms[i];
            }
        }
        return null;
    },

    //find device
    findDevice: function(uuid)
    {
        var self = this;
        var devices = self.devices();
        for ( var i=0; i< devices.length; i++)
        {
            if( devices[i].uuid===uuid )
            {
                return devices[i];
            }
        }
        return null;
    },
    findDeviceByInternalId: function(handledby, internalId)
    {
        // XXX: Why do we expose "internalid" and not just use device IDs?
        var self = this;
        var devices = self.devices();
        for ( var i=0; i< devices.length; i++)
        {
            var dev = devices[i];
            if( dev["handled-by"] === handledby && dev['internalid'] == internalId)
                return dev;
        }
        return null;
    },

    //return specified process or null if not found
    findProcess: function(proc)
    {
        var self = this;
        if( proc && $.trim(proc).length>0 && self.processes().length>0 )
        {
            var processes = self.processes()
            for( var i=0; i< processes.length; i++ )
            {
                if( processes[i].name===proc )
                {
                    //process found
                    return processes[i];
                }
            }
        }
        return null;
    },

    //refresh devices list
    refreshDevices: function()
    {
        var self = this;

        //TODO for now refresh all inventory
        self.getInventory()
            .then(function(result) {
                // XXX: actually deos not update inventory, only devices.
                var devs = self.cleanInventory(result.data.devices);
                for( var uuid in devs )
                {
                    var dev = devs[uuid];
                    self._fixDeviceRoomInfo(dev);

                    var existingDevice = self.findDevice(uuid);
                    if(existingDevice)
                    {
                        // device already exists in devices array. Update its content
                        existingDevice.update(dev, uuid);
                    }
                    else
                    {
                        //add new device
                        self.devices.push(new device(self, dev, uuid));
                        self.inventory.devices[uuid] = dev;
                    }
                }
            });
    },

    //refresh dashboards list
    refreshDashboards: function()
    {
        var self = this;

        //TODO for now refresh all inventory
        self.getInventory()
            .then(function(result){
                self.handleDashboards(result.data.floorplans);
            });
    },

    //get inventory
    getInventory: function()
    {
        var self = this;
        var content = {};
        content.command = "inventory";
        return self.sendCommand(content);
    },

    //get journal entries for today
    getJournalEntries: function()
    {
        var self = this;
        if( self.dataLoggerController )
        {
            var content = {};
            content.uuid = self.journal;
            content.command = 'getmessages';
            content.type = 'all';
            content.filter = '';
            self.sendCommand(content)
                .then(function(res) {
                    //save entries
                    self.journalEntries(res.data.messages);

                    //get journal status
                    var jStatus = 1; //0=debug 1=info 2=warning 3=error
                    for( var i=0; i<res.data.messages.length; i++ )
                    {
                        if( res.data.messages[i].type==="error" )
                        {
                            //max status, stop statement
                            jStatus = 3;
                            break;
                        }
                        else if( res.data.messages[i].type==="warning" )
                        {
                            jStatus = 2;
                        }
                    }
                    if( jStatus===1 ) self.journalStatus('label label-info');
                    else if( jStatus===2 ) self.journalStatus('label label-warning');
                    else if( jStatus===3 ) self.journalStatus('label label-danger');

                    //add journal entries auto refresh
                    if( !self.intervalJournal  )
                    {
                        self.intervalJournal = window.setInterval(self.getJournalEntries.bind(self), 300000);
                    }
                })
                .catch(function(err) {
                    console.error(err);
                });
        }
        else
        {
            console.warn('No datalogger found!');
        }
    },

    /**
     * Internal helper function to add new variables to the observableArray 'variables'.
     * Note that this should not be used to create new variables in the system!
     *
     * @param name
     * @param value
     */
    initVariable: function(name, value) {
        var variable = {
            variable: name,
            value: ko.observable(value),
            action: '' //dummy for datatables
        };

        // This translate the string value into a boolean, or explicit null if it is not a boolean.
        // Idea: Add data types to variables instead?
        variable.booleanValue = ko.pureComputed(function() {
            var v = (variable.value() || '').toLowerCase();
            if(v == 'true')
                return true;
            else if(v == 'false')
                return false;
            else
                return null;
        });

        this.variables.push(variable);
    },

    /**
     * Get the value observable for the given variable, or null if not defined.
     *
     * @param variableName
     * @returns Observable | null
     */
    getVariable: function(variableName) {
        var varObj = this.variables.find(
            function(v){
                return v.variable === variableName
            }
        );
        return varObj != null ? varObj.value : null;
    },

    //handle initial inventory call
    handleInventory: function(result)
    {
        var self = this;

        //INVENTORY
        var inv = self.inventory = result.data;

        //rooms
        for( uuid in inv.rooms )
        {
            var room = inv.rooms[uuid];
            room.uuid = uuid;
            room.name = ko.observable(room.name);
            room.action = ''; //dummy for datatables
            self.rooms.push(room);
        }

        //variables
        Object.keys(inv.variables).forEach(function(name) {
            self.initVariable(name, inv.variables[name]);
        });

        //system
        self.system(inv.system);

        //schema
        self.schema(inv.schema);

        //devices
        var devs = self.cleanInventory(inv.devices);
        for( var uuid in devs )
        {
            self._fixDeviceRoomInfo(devs[uuid]);
            self.devices.push(new device(self, devs[uuid], uuid));
        }

        // Handle dashboards/floorplans
        self.handleDashboards(inv.floorplans);

        // Devices loaded, we now have systemController property set..via device
        self.updateProcessList();
    },

    _fixDeviceRoomInfo : function(dev) {
        if (dev.room !== undefined && dev.room) {
            dev.roomUID = dev.room;
            var room = this.findRoom(dev.room);
            if (room) {
                dev.room = room.name();
                return;
            }
        }
        dev.roomUID = null;
        dev.room = "";
    },

    // Handle dashboard-part of inventory
    handleDashboards : function(floorplans) {
        var dashboards = [];

        // localstorage hack for favourite dashboard.. Backend only has uuid->name now..
        var isHome = function(uuid) {
           return localStorage && (localStorage['home_dashboard'] == uuid);
        };

        var noHomeSelected = !localStorage['home_dashboard'];

        dashboards.push({
            name:ko.observable('all'), uuid: 'all',
            ucName:ko.observable('All my devices'),
            safeName: ko.observable('all'),
            action:'',
            editable:false, icon:'fa-th-large',
            isHome: ko.observable(noHomeSelected || isHome('all'))});

        for( uuid in floorplans )
        {
            var dashboard = floorplans[uuid];
            dashboard.uuid = uuid;
            dashboard.action = '';
            dashboard.name = ko.observable(dashboard.name)
            dashboard.ucName = dashboard.name;
            dashboard.safeName = ko.pureComputed(function(){
                return this.name().replace(/[ &\?#]+/g, '_');
            }, dashboard);
            dashboard.editable = true;
            dashboard.isHome = ko.observable(isHome(uuid));
            if( dashboard.icon===undefined )
            {
                dashboard.icon = null;
            }
            dashboards.push(dashboard);
        }
        this.dashboards.replaceAll(dashboards);
    },

    // Fetch process-list from agosystem
    updateProcessList : function() {
        var self = this;
        var content = {};
        content.command = "getprocesslist";
        content.uuid = self.systemController;
        self.sendCommand(content)
            .then(function(res) {
                var values = [];
                for( var procName in res.data )
                {
                    var proc = res.data[procName];
                    proc.name = procName;
                    values.push(proc);
                }
                self.processes.pushAll(values);
            })
            .catch(function(err){
                notif.warning('Unable to get processes list, Applications will not be available: '+getErrorMessage(err));
                self._noProcesses(true);
            });
    },

    updateFavorites: function() {
        var self = this;

        //FAVORITES
        $.ajax({
            url: "cgi-bin/ui.cgi?param=favorites",
            method: "GET"
        }).done(function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply' && res.result==1 )
            {
                self._favorites(res.content);
            }
            else
            {
                if( res.result.error )
                {
                    console.error('Unable to get favorites: '+res.result.error);
                }
                else
                {
                    console.error('Unable to get favorites');
                }
            }
        });
    },

    updateListing: function(){
        var self = this;
        return $.ajax({
            url : "cgi-bin/listing.cgi?get=all",
            method : "GET"
        }).done(function(result) {
            //APPLICATIONS
            var applications = [];
            for( var i=0; i<result.applications.length; i++ )
            {
                var application = result.applications[i];
                if( application.icon===undefined )
                {
                    application.icon = null;
                }
                application.ucName = ucFirst(application.name);
                applications.push(application);
            }

            // Update internal observable
            self._allApplications(applications);

            //PROTOCOLS
            var protocols = [];
            for( var i=0; i<result.protocols.length; i++ )
            {
                var protocol = result.protocols[i];
                if( protocol.icon===undefined )
                {
                    protocol.icon = null;
                }
                protocol.ucName = ucFirst(protocol.name);
                protocols.push(protocol);
            }

            //Update internal observable
            self._allProtocols(protocols);

            //CONFIGURATION PAGES
            self.configurations.replaceAll(result.config);

            //SUPPORTED DEVICES
            self.supported_devices(result.supported)

            //HELP PAGES
            var helps = [];
            for( var i=0; i<result.help.length; i++ )
            {
                var help = result.help[i];
                help.url = null;
                helps.push(help);
            }
            helps.push({name:'Wiki', url:'http://wiki.agocontrol.com/', description:'Get support on agocontrol wiki'});
            helps.push({name:'About', url:'http://www.agocontrol.com/about/', description:'All about agocontrol'});
            self.helps.replaceAll(helps);

            //SERVER TIME
            self.serverTime = result.server_time;
            self.serverTimeUi( timestampToString(self.serverTime) );
            window.setInterval(function() {
                self.serverTime += 60;
                self.serverTimeUi( timestampToString(self.serverTime) );
            }, 60000);
        });
    },

    getApplication: function(appName) {
        var self = this;
        return this._getApplications.promise
            .then(function(){
                var apps = self.applications();
                for(var i=0; i < apps.length; i++) {
                    if(apps[i].name == appName)
                    {
                        return apps[i];
                    }
                }
                return null;
            });
    },

    getProtocol: function(protocolName) {
        var self = this;
        return this._getProtocols.promise
            .then(function() {
                var pros = self.protocols();
                for( var i=0; i<pros.length; i++ )
                {
                    if( pros[i].name==protocolName )
                    {
                        return pros[i];
                    }
                }
                return null;
            });
    },

    getDashboard:function(safeName){
        return this.dashboards.findByKey('safeName', safeName);
    },

    //get event
    getEvent: function()
    {
        var self = this;

        // Long-poll for events
        var request = {};
        request.method = "getevent";
        request.params = {};
        request.params.uuid = self.subscription;
        request.id = 1;
        request.jsonrpc = "2.0";

        //$.post(url, JSON.stringify(request), null, "json")
        $.ajax({
            type: 'POST',
            url: self.url,
            data: JSON.stringify(request),
            dataType: 'json',
            success: function(data, textStatus, jqXHR)
            {
                //request succeed
                if( data.error!==undefined )
                {
                    if(data.error.code == -32602)
                    {
                        // Subscription not found, server restart or we've been gone
                        // for too long. Setup new subscription
                        self.subscribe();
                        return;
                    }

                    // request timeout (server side), continue polling
                    try {
                        self.handleEvent(false, data);
                    }finally{
                        self.getEvent();
                    }
                }
                else
                {
                    try {
                        self.handleEvent(true, data);
                    }finally{
                        self.getEvent();
                    }
                }
            },
            error: function(jqXHR, textStatus, errorThrown)
            {
                //request failed, retry in a bit
                setTimeout(function()
                {
                    self.getEvent();
                }, 1000);
            }
        });
    },

    //handle event
    handleEvent: function(requestSucceed, response)
    {
        var self = this,
            result = response.result

        if(!requestSucceed )
            return;

        //send event to other handlers
        for( var i=0; i<self.eventHandlers.length; i++ )
        {
            self.eventHandlers[i](result);
        }

        //remove device from inventory
        if( result.event=="event.device.remove" )
        {
            //remove thumb request if device is multigraph
            for( var i=0; i<self.multigraphThumbs.length; i++ )
            {
                if( self.multigraphThumbs[i].uuid===result.uuid )
                {
                    self.multigraphThumbs[i].removed = true;
                }
            }

            //then remove device from inventory
            if( self.inventory && self.inventory.devices && self.inventory.devices[result.uuid] )
            {
                delete self.inventory.devices[result.uuid];
                self.devices.remove(function(item) {
                    return item.uuid===result.uuid;
                });
            }
            else
            {
                console.warn('Unable to delete device "'+result.uuid+'" because it wasn\'t found in inventory');
            }

            return;
        }

        //update device infos if necessary
        if(result.event=="event.device.announce" )
        {
            var uuid = result.uuid;
            if( self.inventory && self.inventory.devices && self.inventory.devices[uuid]===undefined ) {
                // brand new device. Create a basic entry in which a potential 'devicenamechange' can work with
                // Clone and remove event
                var dev = Object.assign({}, result);
                delete dev['event'];

                // Will not have a room yet, but sets blank info..
                self._fixDeviceRoomInfo(dev);

                var existingDevice = self.findDevice(uuid);
                if (!existingDevice) {
                    self.devices.push(new device(self, dev, uuid));
                    self.inventory.devices[uuid] = dev;
                }

                // But then refresh all inventory to ensure we have all details..
                self.refreshDevices();
                /*self.getInventory()
                    .then(function(result) {
                        var tmpDevices = self.cleanInventory(result.data.devices);
                        if( tmpDevices && tmpDevices[result.uuid] )
                        {
                            self.inventory.devices[result.uuid] = tmpDevices[result.uuid];
                        }
                        else
                        {
                            console.warn('Unable to update device because no infos about it in inventory');
                        }
                    });*/
            }

            return;
        }

        //update room name
        if( result.event=="event.system.roomnamechanged" )
        {
            var uuid = result.uuid;
            if( self.inventory && self.inventory.rooms && self.inventory.rooms[uuid]!==undefined ) {
                self.inventory.rooms[uuid].name(result.name);

                // refresh devices with this room
                for (var devUuid in self.inventory.devices) {
                    var dev = self.inventory.devices[devUuid];
                    if (dev.roomUID !== uuid) continue;
                    dev.room = result.name;
                }
            }

            self.devices.forEach(function(dev) {
                if (dev.roomUID !== uuid) return;
                dev.room = result.name;
            });

            return;
        }

        //update device name
        if( result.event=="event.system.devicenamechanged" )
        {
            var uuid = result.uuid;
            if(!self.inventory) return;
            if(self.inventory.devices && self.inventory.devices[uuid]!==undefined ) {
                self.inventory.devices[uuid].name = result.name;
            }

            var dev = self.findDevice(uuid);
            if( dev!==null )
            {
                dev.name(result.name);
            }

            return;
        }

        //handle stale event
        if(result.event=="event.device.stale" )
        {
            if( self.inventory && self.inventory.devices && self.inventory.devices[result.uuid]!==undefined )
            {
                self.inventory.devices[result.uuid].stale = result.stale;
            }

            var dev = self.findDevice(result.uuid);
            if( dev!==null )
            {
                dev.stale(result.stale);
            }

            return;
        }

        //update dashboard name
        if(result.event=="event.system.floorplannamechanged" )
        {
            for( var i=0; i<self.dashboards().length; i++ )
            {
                var dashboard = self.dashboards()[i];
                if( dashboard.uuid && dashboard.uuid===result.uuid )
                {
                    dashboard.name(result.name);
                    dashboard.ucName(result.name);
                    break;
                }
            }

            return;
        }

        var dev = self.findDevice(result.uuid);
        if(dev) {
            //update media infos
            if(result.event=="event.device.mediainfos" )
            {
                if( dev.updateMediaInfos )
                {
                    //update device media infos
                    dev.updateMediaInfos(result);
                }
            }

            // update device last seen datetime
            dev.timeStamp(datetimeToString(new Date()));

            // update device level
            if( result.level !== undefined)
            {
                // update custom device member
                if (result.event.indexOf('event.device') != -1 && result.event.indexOf('changed') != -1)
                {
                    // event that update device member
                    var member = result.event.replace('event.device.', '').replace('changed', '');
                    if (dev[member] !== undefined)
                    {
                        dev[member](result.level);
                    }
                }
                // Binary sensor has its own event
                else if (result.event == "event.security.sensortriggered")
                {
                    if (dev['state'] !== undefined)
                    {
                        dev['state'](result.level);
                    }
                }
            }

            //update device stale
            if( result.event=="event.device.stale" && result.stale!==undefined )
            {
                dev['stale'](result.stale);
            }

            //update quantity
            if (result.quantity)
            {
                var values = dev.values();
                //We have no values so reload from inventory
                if (values[result.quantity] === undefined)
                {
                    self.getInventory()
                        .then(function(result) {
                            var tmpInv = self.cleanInventory(result.data.devices);
                            var uuid = dev.uuid;
                            if (tmpInv[uuid] !== undefined)
                            {
                                if (tmpInv[uuid].values)
                                {
                                    dev.values(tmpInv[uuid].values);
                                }
                            }
                        });
                    // Let getInventory fill up all values.
                    return;
                }

                if( result.level !== undefined )
                {
                   if( result.quantity==='forecast' && typeof result.level=="string" )
                    {
                        //update forecast value for barometer sensor only if string specified
                        dev.forecast(result.level);
                    }
                    //save new level
                    values[result.quantity].level = result.level;
                }
                else if( result.latitude!==undefined && result.longitude!==undefined )
                {
                    values[result.quantity].latitude = result.latitude;
                    values[result.quantity].longitude = result.longitude;
                }

                dev.values(values);
            }
        }
    },

    //add event handler
    //useful to get copy of received event, like in agodrain
    addEventHandler: function(callback)
    {
        var self = this;
        if( callback )
        {
            self.eventHandlers.push(callback);
        }
    },

    //remove specified event handler
    removeEventHandler: function(callback)
    {
        var self = this;
        if( self.eventHandlers.length>0 )
        {
            var index = self.eventHandlers.indexOf(callback);
            if( callback && index!==-1 )
            {
                self.eventHandlers.splice(index, 1);
            }
            else
            {
                console.error('Unable to remove callback from eventHandlers list because callback was not found!');
            }
        }
    },

    //clean inventory
    cleanInventory: function(data)
    {
        var self = this;
        for ( var k in data)
        {
            if (!data[k])
            {
               delete data[k];
            }
        }
        return data;
    },

    subscribe: function()
    {
        var self = this;

        var request = {};
        request.method = "subscribe";
        request.id = 1;
        request.jsonrpc = "2.0";
        $.post(self.url, JSON.stringify(request), self.handleSubscribe.bind(this), "json");
    },

    unsubscribe: function()
    {
        var self = this;

        //TODO fix issue: unsubscribe must be sync

        var request = {};
        request.method = "unsubscribe";
        request.id = 1;
        request.jsonrpc = "2.0";
        request.params = {};
        request.params.uuid = self.subscription;

        $.post(self.url, JSON.stringify(request), function() {}, "json");
    },

    handleSubscribe: function(response)
    {
        var self = this;

        if (response.result)
        {
            self.subscription = response.result;
            self.getEvent();
            window.onbeforeunload = function(event)
            {
                self.unsubscribe();
            };
        }
    },

    //get ui config
    //this function uses cgi to be fast as possible (no need to wait inventory handling)
    getUiConfig: function()
    {
        var self = this;
        return $.ajax({
            url : "cgi-bin/ui.cgi?param=theme",
            method : "GET"
        }).done(function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply' && res.result==1 )
            {
                //apply skin if saved otherwise apply default one
                if( res.content.skin )
                {
                    var skin = res.content.skin;
                    self.skin(skin);

                    //save config to local storage
                    if( typeof(Storage)!=='undefined' && localStorage.getItem('skin')===null )
                    {
                        localStorage.setItem('skin', skin);
                    }
                }
            }
            else
            {
                if( res.result.error )
                {
                    console.error('Unable to get theme: '+res.result.error);
                }
                else
                {
                    console.error('Unable to get theme');
                }
            }
        });
    },

    //get theme from skin
    getThemeFromSkin: function(skin)
    {
        var re = /(skin-\w*).*/g;
        var m = re.exec(skin);
        var theme = 'skin-yellow-light';
        if( m!==null )
        {
            theme = m[1]
        }
        return theme;
    },

    //change ui skin
    setSkin: function(skin)
    {
        var self = this;

        //update theme
        self.theme = self.getThemeFromSkin(skin);

        //build new skin string
        if( self.darkStyle() )
        {
            skin = self.theme+' body-dark';
        }
        else
        {
            skin = self.theme+'-light body-light';
        }

        //check if same skin already applied
        if( skin==self.skin() )
        {
            return;
        }

        //save new skin
        self.skin(skin);

        //save changes
        $.ajax({
            url : "cgi-bin/ui.cgi?key=skin&param=theme&value="+skin,
            method : "GET",
            async : true,
        }).done(function(res) {
            if( !res || !res.result || res.result===0 )
            {
                notif.error('Unable to save skin');
            }
            else
            {
                localStorage.setItem('skin', skin);
                notif.success('Skin saved');
            }
        });
    },

    //set dashboard size
    setDashboardSize: function(size)
    {
        var self = this;

        //save changes
        $.ajax({
            url : "cgi-bin/ui.cgi?key=size&param=theme&value="+size,
            method : "GET",
            async : true,
        }).done(function(res) {
            if( !res || !res.result || res.result===0 )
            {
                notif.error('Unable to save dashboard size');
            }
            else
            {
                if( size=='3x3' )
                {
                    localStorage.setItem('dashboardColSize', 3);
                    localStorage.setItem('dashboardRowSize', 3);
                }
                else if( size=='4x3' )
                {
                    localStorage.setItem('dashboardColSize', 4);
                    localStorage.setItem('dashboardRowSize', 3);
                }
                else if( size=='4x4' )
                {
                    localStorage.setItem('dashboardColSize', 4);
                    localStorage.setItem('dashboardRowSize', 4);
                }
                notif.success('Dashboard size saved. Please refresh page.', 0);
            }
        });
    },

    //collapse/expand menu
    collapseMenu: function(VM, ev)
    {
        var self = VM.agocontrol;
        var collapsed = $('body').hasClass('sidebar-collapse');

        var skin = self.skin();
        skin = skin.replace('sidebar-collapse', '');
        if( !collapsed )
        {
            skin = skin + ' sidebar-collapse';
        }
        self.skin(skin);

        //trigger window resize event (useful for blockly)
        window.setTimeout(function() {
            window.dispatchEvent(new Event('resize'));
        }, 500);

        //save changes
        $.ajax({
            url : "cgi-bin/ui.cgi?key=skin&param=theme&value="+skin,
            method : "GET",
            async : true,
        }).done(function(res) {
            if( !res || !res.result || res.result===0 )
            {
                notif.error('Unable to save skin');
            }
            else
            {
                localStorage.setItem('skin', skin);
            }
        });
    }
};

