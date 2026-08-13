def get_stealth_scripts(profile: dict) -> str:
    """Генерирует JS-payload для подмены отпечатков железа и браузера."""
    return f"""
    Object.defineProperty(navigator, 'webdriver', {{ get: () => undefined }});

    const getParameterProxy = new Proxy(WebGLRenderingContext.prototype.getParameter, {{
        apply: function(target, thisArg, args) {{
            const param = args[0];
            if (param === 37445) return '{profile.get("webgl_vendor", "Intel Inc.")}';
            if (param === 37446) return '{profile.get("webgl_renderer", "Intel Iris OpenGL Engine")}';
            return Reflect.apply(target, thisArg, args);
        }}
    }});
    Object.defineProperty(WebGLRenderingContext.prototype, 'getParameter', {{ value: getParameterProxy }});

    const noise = {profile.get("canvas_noise", 1.000123)};
    const originalFillRect = CanvasRenderingContext2D.prototype.fillRect;
    CanvasRenderingContext2D.prototype.fillRect = function(x, y, w, h) {{
        this.fillStyle = `rgba(0, 0, 0, ${{noise % 0.01}})`;
        originalFillRect.apply(this, [x, y, w, h]);
    }};

    Object.defineProperty(navigator, 'hardwareConcurrency', {{ get: () => {profile.get("cpu_cores", 8)} }});
    Object.defineProperty(navigator, 'deviceMemory', {{ get: () => {profile.get("ram_gb", 8)} }});
    Object.defineProperty(navigator, 'platform', {{ get: () => '{profile.get("platform", "Win32")}' }});
    """
