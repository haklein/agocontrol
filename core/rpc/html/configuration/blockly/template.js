/**
 * Agoblockly plugin
 * @returns {agoblockly}
 */
function agoBlocklyPlugin(devices, agocontrol)
{
    //members
    var self = this;
    self.agocontrol = agocontrol;
    self.luaControllerUuid = null;
    self.availableScripts = ko.observableArray([]);
    self.selectedScript = ko.observable('');
    self.scriptName = ko.observable('untitled');
    self.scriptSaved = ko.observable(true);
    self.scriptLoaded = false;

    //luacontroller uuid
    if( devices!==undefined )
    {
        for( var i=0; i<devices.length; i++ )
        {
            if( devices[i].devicetype=='luacontroller' )
            {
                self.luaControllerUuid = devices[i].uuid;
                break;
            }
        }
    }

    //get unconnected block
    self.getUnconnectedBlock = function() {
        var blocks = Blockly.mainWorkspace.getAllBlocks();
        for (var i = 0, block; block = blocks[i]; i++)
        {
            var connections = block.getConnections_(true);
            for (var j = 0, conn; conn = connections[j]; j++)
            {
                if (!conn.sourceBlock_ || (conn.type == Blockly.INPUT_VALUE || conn.type == Blockly.OUTPUT_VALUE) && !conn.targetConnection)
                {
                    return block;
                }
            }
        }
        return null;
    };
    
    //get block with warning
    self.getBlockWithWarning = function()
    {
        var blocks = Blockly.mainWorkspace.getAllBlocks();
        for (var i = 0, block; block = blocks[i]; i++)
        {
            if (block.warning)
            {
                return block;
            }
        }
        return null;
    };
    
    //blink specified block
    self.blinkBlock = function(block)
    {
        for(var i=300; i<3000; i=i+300)
        {
            setTimeout(function() { block.select(); },i);
            setTimeout(function() { block.unselect(); },i+150);
        }
    };

    //check blocks
    self.checkBlocks = function(notifySuccess) {
        var warningText;
        if( Blockly.mainWorkspace.getTopBlocks(false).length===0 )
        {
            //nothing to save
            notif.info('#nb');
            return;
        }
        var badBlock = self.getUnconnectedBlock();
        if (badBlock)
        {
            warningText = 'This block is not properly connected to other blocks.';
        }
        else
        {
            badBlock = self.getBlockWithWarning();
            if (badBlock)
            {
                warningText = 'Please fix the warning on this block.';
            }
        }

        if (badBlock)
        {
            notif.error(warningText);
            self.blinkBlock(badBlock);
            return false;
        }

        if( notifySuccess )
            notif.success('All blocks seems to be valid');

        return true;
    };

    //merge specified xml and lua code
    self.mergeXmlAndLua = function(xml, lua) {
        var out = '-- /!\\ Lua code generated by agoblockly. Do not edit manually.\n';
        out += '--[[\n' + xml + '\n]]\n';
        out += lua;
        return out;
    };

    //split specified script in xml and lua part
    self.unmergeXmlAndLua = function(script) {
        var out = {'error':false, 'xml':'', 'lua':''};
        //remove line breaks
        script = script.replace(/(\r\n|\n|\r)/gm,"");
        //extract xml and lua
        var re = /--.*--\[\[(.*)\]\](.*)/;
        var result = re.exec(script);
        if( result.length==3 )
        {
            out['xml'] = result[1];
            out['lua'] = result[2];
        }
        else
        {
            out['error'] = true;
        }
        return out;
    };

    //save script
    self.saveScript = function() {
        var scriptName = self.scriptName();
        //replace all whitespaces
        scriptName = scriptName.replace(/\s/g, "_");
        //append blockly_ prefix if necessary
        if( self.scriptName().indexOf('blockly_')!==0 )
        {
            scriptName = 'blockly_'+scriptName;
        }

        var content = {
            uuid: self.luaControllerUuid,
            command: 'setscript',
            name: scriptName,
            script: self.mergeXmlAndLua(self.getXml(), self.getLua())
        };
        self.agocontrol.sendCommand(content, function(res)
        {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                notif.success('#ss');
                self.scriptName(scriptName.replace('blockly_', ''));
                self.scriptSaved(true);
            }
            else
            {
                notif.fatal('#nr', 0);
            }
        });
    };

    //return xml code of blocks
    self.getXml = function() {
        var dom = Blockly.Xml.workspaceToDom(Blockly.mainWorkspace);
        return Blockly.Xml.domToText(dom);
    };

    //set blocks structure
    self.setXml = function(xml) {
        //clear existing blocks
        Blockly.mainWorkspace.clear();
        //load xml
        try {
            var dom = Blockly.Xml.textToDom(xml);
            Blockly.Xml.domToWorkspace(Blockly.mainWorkspace, dom);
            Blockly.mainWorkspace.render();
        }
        catch(e) {
            //exception
            console.log('Exception during xml loading:'+e);
            return false;
        }
        //check if loaded
        if( Blockly.mainWorkspace.getTopBlocks(false).length===0 )
        {
            //no block in workspace
            return false;
        }
        else
        {
            //loaded successfully
            return true;
        }
    };

    //return lua code of blocks
    self.getLua = function() {
        return Blockly.Lua.workspaceToCode();
    };

    //callback when workspace changed
    self.onWorkspaceChanged = function() {
        if( Blockly.mainWorkspace.getTopBlocks(false).length>0 )
        {
            if( !self.scriptLoaded )
            {
                self.scriptSaved(false);
            }
            else if( self.scriptLoaded )
            {
                self.scriptLoaded = false;
            }
        }
    };

    //load scripts
    self.loadScripts = function(callback) {
        //get scripts
        self.agocontrol.block($('#agoGrid'));
        var content = {
            uuid: self.luaControllerUuid,
            command: 'getscriptlist'
        };
        self.agocontrol.sendCommand(content, function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                //update ui variables
                self.availableScripts([]);
                for( var i=0; i<res.result.scriptlist.length; i++ )
                {
                    //only keep agoblockly scripts
                    if( res.result.scriptlist[i].indexOf('blockly_')===0 )
                    {
                        self.availableScripts.push({'name':res.result.scriptlist[i].replace('blockly_','')});
                    }
                }

                //callback
                if( callback!==undefined )
                    callback();
                self.agocontrol.unblock($('#agoGrid'));
            }
            else
            {
                notif.fatal('#nr');
            }
        });
    };

    //load a script
    self.loadScript = function(scriptName, scriptContent) {
        //decode (base64) and extract xml from script content
        var script = self.unmergeXmlAndLua(B64.decode(scriptContent));
        if( !script['error'] )
        {
            if( self.setXml(script['xml']) )
            {
                //remove blockly_ prefix
                if( scriptName.indexOf('blockly_')===0 )
                {
                    scriptName = scriptName.replace('blockly_', '');
                }
                self.scriptName(scriptName);
                self.scriptLoaded = true;
                self.scriptSaved(true);
                notif.success('#sl');
            }
            else
            {
                //script corrupted
                notif.error('#sc');
            }
        }
        else
        {
            //script corrupted
            notif.error('#sc');
        }
    };

    //delete a script
    self.deleteScript = function(script, callback) {
        var content = {
            uuid: self.luaControllerUuid,
            command: 'delscript',
            name: script
        };
        self.agocontrol.sendCommand(content, function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                if( res.result.result===0 )
                {   
                    notif.success('#sd');
                    if( callback!==undefined )
                        callback();
                }
                else
                {
                    notif.error('#nd');
                }
            }
            else
            {
                notif.fatal('#nr');
            }
        });
    };

    //rename a script
    self.renameScript = function(item, oldScript, newScript) {
        var content = {
            uuid: self.luaControllerUuid,
            command: 'renscript',
            oldname: 'blockly_'+oldScript,
            newname: 'blockly_'+newScript
        };
        self.agocontrol.sendCommand(content, function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                if( res.result.result===0 )
                {   
                    item.attr('data-oldname', newScript);
                    notif.success('#rss');
                    self.loadScripts();
                    return true;
                }
                else
                {
                    notif.error('#rsf');
                    return false;
                }
            }
            else
            {
                notif.fatal('#nr');
                return false;
            }
        });
    };

    //Add default blocks
    self.addDefaultBlocks = function() {
        //create blocks
        var ifBlock = Blockly.Block.obtain(Blockly.mainWorkspace, 'controls_if');
        ifBlock.initSvg();
        var contentBlock = Blockly.Block.obtain(Blockly.mainWorkspace, 'agocontrol_content');
        contentBlock.initSvg();
        var eventBlock = Blockly.Block.obtain(Blockly.mainWorkspace, 'agocontrol_eventAll');
        eventBlock.initSvg();

        //connect each others
        ifBlock.getInput('IF0').connection.connect(contentBlock.outputConnection);
        contentBlock.getInput('EVENT').connection.connect(eventBlock.outputConnection);

        //render blocks
        ifBlock.render();
        contentBlock.render();
        eventBlock.render();
    };

    //============================
    //ui events
    //============================

    //clear button
    self.clear = function() {
        if( Blockly.mainWorkspace.getTopBlocks(false).length<2 || window.confirm("Delete everything?") )
        {
            Blockly.mainWorkspace.clear();
            self.scriptName('untitled');
            self.scriptSaved(true);
            self.addDefaultBlocks();
        }
    };

    //check button
    self.check = function() {
        self.checkBlocks(true);
    };

    //save button
    self.save = function() {
        //check code
        if( !self.checkBlocks(false) )
        {
            //TODO allow script saving event if there are errors
            return;
        }

        //request script filename if necessary
        if( self.scriptName()==='untitled' )
        {
            $( "#saveDialog" ).dialog({
                modal: true,
                title: "Save script",
                height: 175,
                width: 375,
                buttons: {
                    "Ok": function() {
                        if( self.scriptName().length===0 || self.scriptName()==='untitled' )
                        {
                            //invalid script name
                            notif.warning('#sn');
                            self.scriptName('untitled');
                            $(this).dialog("close");
                            return;
                        }
                        else
                        {
                            //save script
                            self.saveScript();
                        }
                        $(this).dialog("close");
                    },
                    "Cancel": function() {
                        notif.info('#ns');
                        $(this).dialog("close");
                        return;
                    }
                }
            });
        }
        else
        {
            //script name already specified, save script
            self.saveScript();
        }
    };

    //delete script
    self.delete = function() {
        var content = {
            uuid: self.luaControllerUuid,
            command: 'delscript',
            name: 'TODO'
        };
        self.agocontrol.sendCommand(content, function(res) {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                if( res.result.result===0 )
                {
                    notif.error('#sd');
                }
                else
                {
                    //unable to delete
                    notif.error('#nd');
                }
            }
            else
            {
                notif.fatal('#nr');
            }
        });
    };

    //load code
    self.load = function() {
        //init upload
        $('#fileupload').fileupload({
            dataType: 'json',
            formData: { 
                uuid: self.luaControllerUuid
            },
            done: function (e, data) {
                if( data.result && data.result.result )
                {
                    if( data.result.result.error.len>0 )
                    {
                        notif.error('Unable to import script');
                        console.log('Unable to upload script: '+data.result.result.error);
                    }
                    else
                    {
                        if( data.result.result.count>0 )
                        {
                            $.each(data.result.result.files, function (index, file) {
                                notif.success('Script "'+file.name+'" imported successfully');
                            });
                            self.loadScripts();
                        }
                        else
                        {
                            //no file uploaded
                            notif.error('Script import failed: '+data.result.result.error);
                        }
                    }
                }
                else
                {
                    notif.fatal('Unable to import script: internal error');
                }
            },
            progressall: function (e, data) {
                var progress = parseInt(data.loaded / data.total * 100, 10);
                $('#progress .bar').css('width', progress+'%');
            }
        });

        //load scripts
        self.loadScripts(function()
        {
            //open script dialog
            $( "#loadDialog" ).dialog({
                modal: true,
                title: "Load script",
                height: 700,
                width: 700,
                buttons: {
                    Close: function() {
                        $(this).dialog("close");
                    }
                }
            });
        });
    };

    //view lua source code
    self.viewlua = function() {
        //check code first
        if( !self.checkBlocks(false) )
            return;

        //fill dialog content
        var content = document.getElementById('luaContent');
        var code = self.getLua();
        content.textContent = code;
        if (typeof prettyPrintOne == 'function')
        {
            code = content.innerHTML;
            code = prettyPrintOne(code, 'lang-lua');
            content.innerHTML = code;
        }
        //open dialog
        $( "#luaDialog" ).dialog({
            modal: true,
            title: "LUA script",
            height: 600,
            width: 1024
        });
    };

    //load script
    self.uiLoadScript = function(script) {
        var content = {
            uuid: self.luaControllerUuid,
            command: 'getscript',
            name: 'blockly_'+script
        };
        self.agocontrol.sendCommand(content, function(res)
        {
            if( res!==undefined && res.result!==undefined && res.result!=='no-reply')
            {
                if( res.result.result===0 )
                {
                    self.loadScript(res.result.name ,res.result.script);
                }
                else
                {
                  //error occured
                  notif.error(res.result.error);
                }
            }
            else
            {
               notif.fatal('#nr');
            }
            $("#loadDialog").dialog("close");
        });
    };

    //rename script
    self.uiRenameScript = function(item, td, tr) {
        if( $(td).hasClass('rename_script') )
        {
            $(td).editable(function(value, settings) {
                self.renameScript($(this), $(this).attr('data-oldname'), value);
                return value;
            },
            {
                data : function(value, settings)
                {
                    return value;
                },
                onblur : "cancel"
            }).click();
        }
    };

    self.grid = new ko.agoGrid.viewModel({
        data: self.availableScripts,
        columns: [
            {headerText:'Script', rowText:'name'},
            {headerText:'Actions', rowText:''}
        ],
        rowCallback: self.uiRenameScript,
        rowTemplate: 'rowTemplate'
    });

    //delete script
    self.uiDeleteScript = function(script) {
        var msg = $('#cd').html();
        if( confirm(msg) )
        {
            self.deleteScript('blockly_'+script, function() {
                self.loadScripts();
            });
        }
    };

    //export script
    self.uiExportScript = function(script) {
        downloadurl = location.protocol + "//" + location.hostname + (location.port && ":" + location.port) + "/download?filename="+script+"&uuid="+self.luaControllerUuid;
        window.open(downloadurl, '_blank');
    };

    //view model
    this.blocklyViewModel = new ko.blockly.viewModel({
        onWorkspaceChanged: self.onWorkspaceChanged,
        addDefaultBlocks: self.addDefaultBlocks
    });
}

/**
 * Entry point: mandatory!
 */
function init_template(path, params, agocontrol)
{
    ko.blockly = {
        viewModel: function(config) {
            this.onWorkspaceChanged = config.onWorkspaceChanged;
            this.addDefaultBlocks = config.addDefaultBlocks;
        }
    };

    ko.bindingHandlers.blockly = {
        update: function(element, viewmodel) {
            element.innerHTML = "";
            //inject blockly
            Blockly.inject( document.getElementById('blocklyDiv'), {
                path: "/configuration/agoblockly/blockly/",
                toolbox: document.getElementById('toolbox')
            });
            //init agoblockly
            if( BlocklyAgocontrol!==null && BlocklyAgocontrol.init!==undefined )
            {
                BlocklyAgocontrol.init(agocontrol.schema(), agocontrol.devices(), agocontrol.variables());
                //handle workspace changing event
                Blockly.addChangeListener(viewmodel().onWorkspaceChanged);
            }
            else
            {
                notif.error('Unable to configure Blockly! Event builder shouldn\'t work.');
            }
            //init blocks
            viewmodel().addDefaultBlocks();
        }
    };

    var model = new agoBlocklyPlugin(agocontrol.devices(), agocontrol);

    return model;
}