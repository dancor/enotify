//
// Cross-browser DOMContentLoaded event
var contentLoadedHooks = [];
function onDOMContentLoaded(fn) {
  if (window.loaded) {
    fn();
  } else {
    contentLoadedHooks.push(fn);
  }
}
function DOMContentLoaded() {
  if (window.loaded) {
    return;
  }
  window.loaded = true;
  contentLoadedHooks.invoke('call');
  EventNotifier.init();
  var nodes = document.body.getElementsByTagName('script'), node;
  while (node = nodes[0]) {
    node.parentNode.removeChild(node);
  }
}

if (/WebKit/i.test(navigator.userAgent)) {
  var _onloadTimer = setInterval(function() {
    if (/loaded|complete/.test(document.readyState)) {
      clearInterval(_onloadTimer);
      DOMContentLoaded();
    }
  }, 10);
} else if (document.addEventListener) {
  document.addEventListener('DOMContentLoaded', DOMContentLoaded, false);
} else {
  document.write('<script onreadystatechange="if(this.readyState==\'complete\')DOMContentLoaded()" defer="defer" src="javascript:void(0)"></script>');
}

//
// Event utility functions
function chain(left, right) {
  return function() {
    return (left && left.apply(this, arguments) === false || right && right.apply(this, arguments) === false) ? true : undefined;
  }
}

function addEventListener(obj, type, fn) {
  if (obj.addEventListener) {
    obj.addEventListener(type, fn, false);
  } else if (obj.attachEvent) {
    obj.attachEvent('on'+type, fn);
  } else {
    obj['on'+type] = chain(obj['on'+type], fn);
  }
}

//
// DOM utility functions
function $(i) {
  return document.getElementById(i);
}

var DOM = {
  eval: function(node) {
    var sc = node.getElementsByTagName('script'), ii;
    if (DOM.scriptsEval === null) {
      DOM.scriptsEval = false;
      var tmp = document.createElement('div');
      tmp.innerHTML = '<script>DOM.scriptsEval = true</script>';
      document.body.removeChild(document.body.appendChild(tmp));
    }
    while (ii = sc[0]) {
      if (!DOM.scriptsEval) {
        var src = ii.innerHTML,
          ev = document.createElement('script');
        ev.type = 'text/javascript';
        try {
          ev.appendChild(document.createTextNode(src));
        } catch(e) {
          ev.text = src;
        }
        document.body.appendChild(ev).parentNode.removeChild(ev);
      }
      ii.parentNode.removeChild(ii);
    }
  },
  scriptsEval: null,
  getCaret: function(node) {
    if ('selectionStart' in node) {
      return {
        start: node.selectionStart,
        end: node.selectionEnd
      }
    } else {
      try {
        var range = document.selection.createRange(),
          shadowRange = range.duplicate();
        shadowRange.moveToElementText(node);
        shadowRange.setEndPoint('StartToEnd', range);
        var end = node.value.length - shadowRange.text.length;
        shadowRange.setEndPoint('StartToStart', range);
        return {
          start: node.value.length - shadowRange.text.length,
          end: end
        };
      } catch(e) {
        return {
          start: 0,
          end: 0
        };
      }
    }
  },
  setCaret: function(node, start, end) {
    if (end === void 0) {
      end = start;
    } else {
      end = Math.min(node.value.length, end);
    }
    if ('selectionStart' in node) {
      node.focus();
      node.selectionStart = start;
      node.selectionEnd = end;
    } else {
      if (node.tagName === 'TEXTAREA') {
        var ii = node.value.indexOf("\r", 0);
        while (ii != -1 && ii < end) {
          --end;
          if (ii < start) {
            --start;
          }
          ii = node.value.indexOf("\r", ii + 1);
        }
      }
      var range = node.createTextRange();
      range.collapse(true);
      range.moveStart('character', start);
      if (end != undefined) {
        range.moveEnd('character', end - start);
      }
      range.select();
    }
  },
  serializeForm: function(form) {
    var ret = {};
    $A(form.elements).forEach(function(ii) {
      if (ii.tagName != 'INPUT' || (ii.type != 'submit' && ii.type != 'button')) {
        ret[ii.name] = ii.value;
      }
    });
    return ret;
  }
}

var CSS = {
  hasClass: function(node, cl) {
    return new RegExp('(?:^|\\s)' + cl + '(?:\\s|$)').test(node.className);
  },
  addClass: function(node, cl) {
    CSS.hasClass(node, cl) || (node.className += ' ' + cl);
  },
  removeClass: function(node, cl) {
    node.className = node.className.replace(new RegExp('(?:^|\\s)' + cl + '(?:\\s|$)', 'g'), '');
  },
  toggleClass: function(node, cl) {
    (CSS.hasClass(node, cl) ? CSS.removeClass : CSS.addClass)(node, cl);
  },
  getComputedStyle: function(node) {
    if (window.getComputedStyle) {
      return getComputedStyle(node, null);
    }
    if (document.defaultView && document.defaultView.getComputedStyle) {
      return document.defaultView.getComputedStyle(node, null);
    }
    if (node.currentStyle) {
      return node.currentStyle;
    }
    return node.style;
  }
}

//
// Pretty good version of json_encode
function json_encode(obj) {
  var ret = [];
  if (obj instanceof Array) {
    for (var i = 0; i < obj.length; i++) {
      ret.push(json_encode(obj[i]));
    }
    return '['+ret.join(',')+']';
  } else if (typeof obj == 'object') {
    for (var i in obj) {
      ret.push('"' + i + '":' + json_encode(obj[i]));
    }
    return '{'+ret.join(',')+'}';
  } else if (typeof obj == 'string') {
    return '"'+obj.replace(/"/g, '\\\"').replace(/\\/g, '\\\\')+'"';
  } else {
    return ''+obj;
  }
}

//
// Function extensions
Function.prototype.bind = function(context) {
  var fn = this,
    args = arguments.length > 1 ? [].slice.call(arguments, 1) : null;
  return function() {
    return fn.apply(context, args ? args.concat([].slice.call(arguments)) : arguments);
  }
}

Function.prototype.bindShift = function(context) {
  var fn = this,
    args = arguments.length > 1 ? [].slice.call(arguments, 1) : null;
  return function() {
    return fn.apply(context, args ? [this].concat(args).concat([].slice.call(arguments)) : [this].concat([].slice.call(arguments)));
  }
}

Function.prototype.defer = function(timeout) {
  return setTimeout(this, timeout ? timeout : 0);
}

//
// Array extensions
function $M(v, u) {
  for (var i in u) {
    v[i] = u[i];
  }
  return v;
}
function $A(penum) {
  if (penum instanceof Array) {
    return penum.slice();
  } else {
    var ret = [];
    for (var i = 0; i < penum.length; i++) {
      ret.push(penum[i]);
    }
    return ret;
  }
}

Array.prototype.last = function() {
  return this[this.length - 1];
}

Array.prototype.pull = function(grip) {
  var ret = [];
  for (var i = 0; i < this.length; ++i) {
    ret.push(this[i][grip]);
  }
  return ret;
}

Array.prototype.invoke = function(method) {
  var ret = [], args = [];
  for (var i = 1; i < arguments.length; ++i) {
    args.push(arguments[i]);
  }
  for (var i = 0; i < this.length; ++i) {
    ret.push(this[i][method].apply(this[i], args));
  }
  return ret;
}

Array.prototype.filter = Array.prototype.filter || function(fn) {
  var ret = [];
  for (var i = 0; i < this.length; ++i) {
    if (fn.call(this, this[i], i)) {
      ret.push(this[i]);
    }
  }
  return ret;
}

Array.prototype.forEach = Array.prototype.forEach || function(fn, thisp) {
  for (var i = 0; i < this.length; ++i) {
    fn.call(thisp, this[i], i, this);
  }
}

//
// Subscriber interface
function Subscriber(focus) {
  this.focus = focus;
  this.events = {};
  focus.subscribe = this.subscribe.bind(this);
  focus.publish = this.publish.bind(this);
}

Subscriber.prototype.subscribe = function(event, fn) {
  (this.events[event] || (this.events[event] = [])).push(fn);
}

Subscriber.prototype.publish = function(event) {
  var ret;
  if (this.events[event]) {
    ret = this.events[event].invoke('apply', this.focus, [null].concat([].slice.call(arguments, 1))).filter(function(ii) {
      return ii === false;
    });
    return !ret.length;
  }
  return true;
}

//
// Ajax class
function Ajax(uri) {
  if (this === window) {
    return new Ajax(uri);
  }
  this.uri = uri;
  this.domain = (/https?:\/\/(.+?)\//.exec(uri) || [document.location.host]).pop();
}
Ajax.data = {
  form: 1,
  raw: 2
}
Ajax.domains = {}
Ajax.queuedSend = [];
Ajax.counter = 0;

Ajax.getXMLHttpRequest = function(domain) {
  var root;
  if (domain == document.location.host) {
    try {
      return new XMLHttpRequest();
    } catch(e) {
      try {
        return new ActiveXObject('Msxml2.XMLHTTP');
      } catch(e) {
        try {
          return new ActiveXObject('Microsoft.XMLHTTP');
        } catch(e) {
          return false;
        }
      }
    }
  } else if (Ajax.domains[domain]) {
    return new Ajax.domains[domain];
  } else {
    if (/Firefox\/1\.0/.test(navigator.userAgent)) {
      // No cross-domain due to Firefox 1.0's document.domain bug:
      // https://bugzilla.mozilla.org/show_bug.cgi?id=290100
      // It could be built using an iframe bridge, but it's not worth the time.
      return false;
    }
    var proto = document.location.protocol;
    var rootDomain = /[^.]+\.[^.]+$/.exec(document.location.host);
    if (rootDomain != document.domain || !window.ActiveXObject) {
      document.domain = rootDomain;
    }
    var iframe = document.createElement('iframe');
    iframe.style.position = 'absolute';
    iframe.style.top = iframe.style.left = '-1000px';
    iframe.src = document.location.protocol + '//' + domain + '/crossdomain.html';
    document.body.appendChild(iframe);
    return true;
  }
}

Ajax.formEncodeData = function(data, idx) {
  var buf = [];

  if (data instanceof Array) {
    for (var i = 0; i < data.length; i++) {
      buf.push(Ajax.formEncodeData(data[i], idx ? idx+'['+i+']' : i));
    }
  } else if (typeof data == 'object') {
    for (var i in data) {
      buf.push(Ajax.formEncodeData(data[i], idx ? idx+'['+i+']' : i));
    }
  } else if (idx) {
    buf.push(encodeURIComponent(idx)+'='+encodeURIComponent(data));
  } else {
    buf.push(encodeURIComponent(data));
  }
  return buf.join('&');
}

Ajax.addUnloadAbort = function() {
  if (Ajax.unloadAbort) {
    return;
  }
  Ajax.unloadAbort = true;

  window.onbeforeunload = chain(window.onbeforeunload, function() {
    for (var i = 0; i < Ajax.active.length; i++) {
      Ajax.active[i].abort();
    }
  });
}

Ajax.subdomainReady = function(domain, XHR) {
  Ajax.domains[domain] = XHR;

  Ajax.queuedSend.filter(function(ajax) {
    if (ajax.domain == domain) {
      ajax.send();
      return false;
    } else {
      return true;
    }
  });
}

Ajax.prototype.data = function(data, type) {
  this._data = data;
  this.dataType = type || Ajax.data.form;
  return this;
}

Ajax.prototype.abort = function() {
  try {
    this.req.abort();
  } catch(e) {}
  this.setActive(false);
  return this;
}

Ajax.prototype.onsuccess = function(handler) {
  this.onsuccess_handler = handler;
  return this;
}

Ajax.prototype.onerror = function(handler) {
  this.onerror_handler = handler;
  return this;
}

Ajax.prototype.onfinally = function(handler) {
  this.onfinally_handler = handler;
  return this;
}

Ajax.active = [];
Ajax.prototype.setActive = function(flag) {
  if (flag) {
    Ajax.active.push(this);
  } else {
    Ajax.active = Ajax.active.filter(function(a) {
      return (a != this);
    }.bind(this));
    this.req = null;
  }
}

Ajax.prototype.send = function() {
  this.req = Ajax.getXMLHttpRequest(this.domain);

  if (this.req === false) {
    return false;
  } else if (this.req === true) {
    if (this.triedCrossdomain) {
      return false;
    } else {
      this.triedCrossdomain = true;
      Ajax.queuedSend.push(this);
      return true;
    }
  }

  this.req.onreadystatechange = function() {
    if (this.req.readyState == 4) {
      (function() {
        var ex = null;
        try {
          if (this.req && this.req.status == 200) {
            var response = this.req.responseText;
            // TODO: json_decode?
            eval('response='+(response.substr(0, 1) == '}' ? response.substr(1) : response));
            if (response['error']) {
              if (this.onerror_handler) {
                this.onerror_handler(response['error']);
              } else {
                //console.log('Unhandled AJAX error: ' + response['error']);
              }
            } else if (response['payload']) {
              this.onsuccess_handler && this.onsuccess_handler(response['payload']);
            } else {
              if (this.onerror_handler) {
                this.onerror_handler('AJAX: Empty response returned.');
              } else {
                //console.log('Unhandled AJAX error: empty response returned.');
              }
            }
          } else {
            this.onerror_handler && this.onerror_handler();
          }
        } catch(e) {
          ex = e;
          this.onerror_handler && this.onerror_handler();
        }
        this.onfinally_handler && this.onfinally_handler();
        this.setActive(false);
        if (ex !== null) {
          throw ex;
        }
      }).bind(this).defer();
    }
  }.bind(this);

  this.req.open('POST', this.uri, true);
  if (this.dataType == Ajax.data.raw) {
    this.req.send(this._data);
  } else {
    this.req.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    this.req.send(Ajax.formEncodeData($M(this._data || {}, {
      '-ajaxCounter': ++Ajax.counter
    })));
  }
  this.setActive(true);
  Ajax.addUnloadAbort();
  return true;
}

//
// EventNotifier class
function EventNotifier() {
}
EventNotifier.events = {};
EventNotifier.errorTimeout = 65000;

EventNotifier.register = function(eid, value, callback, extra) {
  EventNotifier.events[eid] = {
    value: value,
    callback: callback,
    extra: extra
  };
}

EventNotifier.init = function() {

}

EventNotifier.onDataReceived = function(response) {
  window.clearTimeout(EventNotifier.errorTimeoutID);
  EventNotifier.errorTimeoutID = null;

  for (var i in response) {
    if (EventNotifier.events[i]) {
      EventNotifier.events[i].callback(response[i], EventNotifier.events[i].value, EventNotifier.events[i].extra);
      EventNotifier.events[i].value = response[i];
    }
  }
  EventNotifier.subscribe();
}



EventNotifier.subscribe = function() {
  var qstr = 'jsonp=EventNotifier.onDataReceived(%251%25)';
  for (var i in EventNotifier.events) {
    qstr = qstr + '&' + i + '=' + EventNotifier.events[i].value;
  }

  EventNotifier.errorTimeoutID = EventNotifier.subscribe.defer(EventNotifier.errorTimeout);
  var scripttag = document.createElement('script');
  scripttag.src = 'http://mantup.com:9090/subscribe?' + qstr;

  $('jsonp_container').innerHTML = '';

  $('jsonp_container').appendChild(scripttag);
}

