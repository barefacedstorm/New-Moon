// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
// ==UserScript==
// @name         ES2025+ Standard Library Polyfill (New Moon)
// @namespace    internal-userscripts
// @description  Adds Promise.try, ES2025 Set methods, RegExp.escape, and Error.isError when missing.
// @match        *://*/*
// @grant        none
// @run-at       document-start
// ==/UserScript==
(function () {
  "use strict";

  var global = typeof window !== "undefined" ? window : null;
  if (!global) {
    return;
  }

  function defineMethod(target, name, fn) {
    if (target && typeof target[name] !== "function") {
      Object.defineProperty(target, name, {
        value: fn,
        writable: true,
        enumerable: false,
        configurable: true
      });
    }
  }

  // 1. Promise.try (ES2025)
  if (typeof global.Promise === "function") {
    defineMethod(global.Promise, "try", function (callbackFn) {
      var C = this;
      if (typeof C !== "function") {
        throw new TypeError("Promise.try requires a constructor receiver");
      }
      var args = [];
      for (var i = 1; i < arguments.length; i++) {
        args.push(arguments[i]);
      }
      return new C(function (resolve, reject) {
        try {
          resolve(callbackFn.apply(undefined, args));
        } catch (err) {
          reject(err);
        }
      });
    });
  }

  // 2. ES2025 Set methods
  if (typeof global.Set === "function" && global.Set.prototype) {
    var SetCtor = global.Set;
    var SetProto = SetCtor.prototype;

    function getSetRecord(obj) {
      if (obj === null || (typeof obj !== "object" && typeof obj !== "function")) {
        throw new TypeError("Argument must be an object");
      }
      var rawSize = Number(obj.size);
      if (isNaN(rawSize) || rawSize < 0) {
        throw new RangeError("Set-like object has invalid size");
      }
      if (typeof obj.has !== "function") {
        throw new TypeError("Set-like object missing has() method");
      }
      if (typeof obj.keys !== "function") {
        throw new TypeError("Set-like object missing keys() method");
      }
      return { set: obj, size: rawSize, has: obj.has, keys: obj.keys };
    }

    function forEachKey(rec, cb) {
      var iter = rec.keys.call(rec.set);
      if (!iter || typeof iter.next !== "function") {
        throw new TypeError("keys() did not return an iterator");
      }
      var step;
      while (!(step = iter.next()).done) {
        if (cb(step.value) === false) {
          if (typeof iter.return === "function") {
            try { iter.return(); } catch (e) {}
          }
          break;
        }
      }
    }

    defineMethod(SetProto, "union", function union(other) {
      var rec = getSetRecord(other);
      var out = new SetCtor(this);
      forEachKey(rec, function (v) { out.add(v); });
      return out;
    });

    defineMethod(SetProto, "intersection", function intersection(other) {
      var rec = getSetRecord(other);
      var out = new SetCtor();
      if (this.size <= rec.size) {
        this.forEach(function (v) {
          if (rec.has.call(rec.set, v)) {
            out.add(v);
          }
        });
      } else {
        var self = this;
        forEachKey(rec, function (v) {
          if (self.has(v)) {
            out.add(v);
          }
        });
      }
      return out;
    });

    defineMethod(SetProto, "difference", function difference(other) {
      var rec = getSetRecord(other);
      var out = new SetCtor(this);
      if (this.size <= rec.size) {
        this.forEach(function (v) {
          if (rec.has.call(rec.set, v)) {
            out.delete(v);
          }
        });
      } else {
        forEachKey(rec, function (v) {
          out.delete(v);
        });
      }
      return out;
    });

    defineMethod(SetProto, "symmetricDifference", function symmetricDifference(other) {
      var rec = getSetRecord(other);
      var out = new SetCtor(this);
      var self = this;
      forEachKey(rec, function (v) {
        if (self.has(v)) {
          out.delete(v);
        } else {
          out.add(v);
        }
      });
      return out;
    });

    defineMethod(SetProto, "isSubsetOf", function isSubsetOf(other) {
      var rec = getSetRecord(other);
      if (this.size > rec.size) {
        return false;
      }
      var result = true;
      this.forEach(function (v) {
        if (result && !rec.has.call(rec.set, v)) {
          result = false;
        }
      });
      return result;
    });

    defineMethod(SetProto, "isSupersetOf", function isSupersetOf(other) {
      var rec = getSetRecord(other);
      if (this.size < rec.size) {
        return false;
      }
      var self = this;
      var result = true;
      forEachKey(rec, function (v) {
        if (!self.has(v)) {
          result = false;
          return false;
        }
      });
      return result;
    });

    defineMethod(SetProto, "isDisjointFrom", function isDisjointFrom(other) {
      var rec = getSetRecord(other);
      var result = true;
      if (this.size <= rec.size) {
        this.forEach(function (v) {
          if (result && rec.has.call(rec.set, v)) {
            result = false;
          }
        });
      } else {
        var self = this;
        forEachKey(rec, function (v) {
          if (self.has(v)) {
            result = false;
            return false;
          }
        });
      }
      return result;
    });
  }

  // 3. RegExp.escape (ES2025)
  if (typeof global.RegExp === "function") {
    var SYNTAX_RE = /[\\^$.*+?()[\]{}|\/]/;
    var CONTROL_MAP = {
      "\t": "\\t",
      "\n": "\\n",
      "\v": "\\v",
      "\f": "\\f",
      "\r": "\\r"
    };
    var OTHER_PUNCT_RE = /[,-=<>#&!%:;@~'`"]/;

    defineMethod(global.RegExp, "escape", function escape(S) {
      if (typeof S !== "string") {
        throw new TypeError("RegExp.escape requires a string argument");
      }
      var out = "";
      for (var i = 0; i < S.length; i++) {
        var ch = S.charAt(i);
        var code = S.charCodeAt(i);
        if (i === 0 && ((code >= 0x30 && code <= 0x39) || (code >= 0x41 && code <= 0x5A) || (code >= 0x61 && code <= 0x7A))) {
          out += "\\x" + code.toString(16);
        } else if (SYNTAX_RE.test(ch)) {
          out += "\\" + ch;
        } else if (CONTROL_MAP[ch]) {
          out += CONTROL_MAP[ch];
        } else if (OTHER_PUNCT_RE.test(ch) || code === 0x20 || code === 0xA0) {
          out += "\\x" + (code < 16 ? "0" : "") + code.toString(16);
        } else if (code === 0x2028 || code === 0x2029) {
          out += "\\u" + code.toString(16);
        } else {
          out += ch;
        }
      }
      return out;
    });
  }

  // 4. Error.isError (ES2025/2026)
  if (typeof global.Error === "function") {
    var objToString = Object.prototype.toString;
    defineMethod(global.Error, "isError", function isError(arg) {
      if (arg === null || typeof arg !== "object") {
        return false;
      }
      if (arg instanceof global.Error) {
        return true;
      }
      var tag = objToString.call(arg);
      return tag === "[object Error]" || tag === "[object DOMException]";
    });
  }

  try {
    global.__internalUserscriptsES2025Polyfill = true;
  } catch (e) {}
})();
