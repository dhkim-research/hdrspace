document.addEventListener("DOMContentLoaded", () => {
  const ui = {
    navItems: Array.from(document.querySelectorAll(".nav-item")),
    panels: Array.from(document.querySelectorAll(".panel-view")),

    workdir: document.getElementById("workdir"),
    pickWorkdir: document.getElementById("pickWorkdir"),
    inputPattern: document.getElementById("inputPattern"),
    pickInputFiles: document.getElementById("pickInputFiles"),
    inputFilesList: document.getElementById("inputFilesList"),
    outputDir: document.getElementById("outputDir"),
    pickOutputDir: document.getElementById("pickOutputDir"),
    outputName: document.getElementById("outputName"),
    profile: document.getElementById("profile"),
    outputFormat: document.getElementById("outputFormat"),
    colorspace: document.getElementById("colorspace"),
    toggleColorInfo: document.getElementById("toggleColorInfo"),
    colorInfo: document.getElementById("colorInfo"),
    previewSource: document.getElementById("previewSource"),
    pickPreviewSource: document.getElementById("pickPreviewSource"),
    blackLevel: document.getElementById("blackLevel"),
    whiteLevel: document.getElementById("whiteLevel"),
    saturation: document.getElementById("saturation"),
    cropX: document.getElementById("cropX"),
    cropY: document.getElementById("cropY"),
    cropW: document.getElementById("cropW"),
    cropH: document.getElementById("cropH"),
    generatePreview: document.getElementById("generatePreview"),
    clearCrop: document.getElementById("clearCrop"),
    bloom: document.getElementById("bloom"),
    fisheye: document.getElementById("fisheye"),
    demosaic: document.getElementById("demosaic"),
    mergeWeight: document.getElementById("mergeWeight"),
    rawgrid: document.getElementById("rawgrid"),
    xyzcamOverride: document.getElementById("xyzcamOverride"),
    extra: document.getElementById("extra"),
    startMerge: document.getElementById("startMerge"),
    openTev: document.getElementById("openTev"),
    mergeStatus: document.getElementById("mergeStatus"),
    progressFill: document.getElementById("progressFill"),
    progressPercent: document.getElementById("progressPercent"),
    progressStage: document.getElementById("progressStage"),
    luminanceSummary: document.getElementById("luminanceSummary"),
    issuesBox: document.getElementById("issuesBox"),
    copyCommand: document.getElementById("copyCommand"),
    commandOut: document.getElementById("commandOut"),
    logOut: document.getElementById("logOut"),

    previewMeta: document.getElementById("previewMeta"),
    previewStage: document.getElementById("previewStage"),
    previewCanvas: document.getElementById("previewCanvas"),
    previewImage: document.getElementById("previewImage"),
    viewerEmpty: document.getElementById("viewerEmpty"),
    fitView: document.getElementById("fitView"),
    actualPixels: document.getElementById("actualPixels"),
    modeCrop: document.getElementById("modeCrop"),
    modePan: document.getElementById("modePan"),
    viewerExposure: document.getElementById("viewerExposure"),
    viewerGamma: document.getElementById("viewerGamma"),
    viewerFalseColor: document.getElementById("viewerFalseColor"),
    viewerModeLabel: document.getElementById("viewerModeLabel"),
    viewerZoomLabel: document.getElementById("viewerZoomLabel"),
    viewerReadout: document.getElementById("viewerReadout"),

    calWorkdir: document.getElementById("calWorkdir"),
    calReference: document.getElementById("calReference"),
    calTest: document.getElementById("calTest"),
    pickCalReference: document.getElementById("pickCalReference"),
    pickCalTest: document.getElementById("pickCalTest"),
    detectRefCells: document.getElementById("detectRefCells"),
    detectTestCells: document.getElementById("detectTestCells"),
    calRefCells: document.getElementById("calRefCells"),
    calTestCells: document.getElementById("calTestCells"),
    calRefcol: document.getElementById("calRefcol"),
    calMinimizer: document.getElementById("calMinimizer"),
    calVerbose: document.getElementById("calVerbose"),
    calXyzcam: document.getElementById("calXyzcam"),
    runColorCalibrate: document.getElementById("runColorCalibrate"),
    copyRecommendedXyzcam: document.getElementById("copyRecommendedXyzcam"),
    recommendedMatrix: document.getElementById("recommendedMatrix"),
    chartMeta: document.getElementById("chartMeta"),
    chartPreviewImage: document.getElementById("chartPreviewImage"),
    chartCellsText: document.getElementById("chartCellsText"),
    calibrationResult: document.getElementById("calibrationResult"),

    shutterWorkdir: document.getElementById("shutterWorkdir"),
    shutterSequences: document.getElementById("shutterSequences"),
    shutterCrops: document.getElementById("shutterCrops"),
    shutterChannel: document.getElementById("shutterChannel"),
    shutterDataout: document.getElementById("shutterDataout"),
    useMergeCropForShutter: document.getElementById("useMergeCropForShutter"),
    runShutter: document.getElementById("runShutter"),
    shutterResult: document.getElementById("shutterResult"),

    apertureWorkdir: document.getElementById("apertureWorkdir"),
    apertureSequences: document.getElementById("apertureSequences"),
    apertureCrop: document.getElementById("apertureCrop"),
    apertureShutterc: document.getElementById("apertureShutterc"),
    useMergeCropForAperture: document.getElementById("useMergeCropForAperture"),
    runAperture: document.getElementById("runAperture"),
    apertureResult: document.getElementById("apertureResult"),

    settingsMergehdrPath: document.getElementById("settingsMergehdrPath"),
    settingsTevPath: document.getElementById("settingsTevPath"),
    settingsOiiotoolPath: document.getElementById("settingsOiiotoolPath"),
    pickSettingsMergehdr: document.getElementById("pickSettingsMergehdr"),
    pickSettingsTev: document.getElementById("pickSettingsTev"),
    pickSettingsOiiotool: document.getElementById("pickSettingsOiiotool"),
    saveSettings: document.getElementById("saveSettings"),
    useDetectedPaths: document.getElementById("useDetectedPaths"),
    settingsProfilesInfo: document.getElementById("settingsProfilesInfo"),
  };

  const state = {
    info: null,
    selectedInputFiles: [],
    currentJobId: null,
    pollTimer: null,
    latestOutput: "",
    latestChart: null,
    recommendedXyzcam: "",
    detectedPaths: { mergehdr: "", tev: "", oiiotool: "" },
    profileDefaults: {},
    preview: null,
    viewer: {
      mode: "crop",
      zoom: 1,
      panX: 0,
      panY: 0,
      preset: "fit",
      exposure: 0,
      gamma: 1,
      falseColor: false,
      drag: null,
      hover: null,
      sourceData: null,
      processedCanvas: null,
    },
  };

  const ctx = ui.previewCanvas.getContext("2d");

  const asObject = (value) => (typeof value === "string" ? JSON.parse(value) : value);
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
  const dirname = (value) => {
    if (!value) return "";
    const normalized = value.replace(/\\/g, "/");
    const slash = normalized.lastIndexOf("/");
    return slash > 0 ? normalized.slice(0, slash) : "";
  };

  const setPanel = (name) => {
    ui.navItems.forEach((item) => item.classList.toggle("active", item.dataset.panel === name));
    ui.panels.forEach((panel) => panel.classList.toggle("active", panel.id === `panel-${name}`));
  };

  const queryFromObject = (pairs) => {
    const params = new URLSearchParams();
    Object.entries(pairs).forEach(([key, value]) => {
      if (value !== undefined && value !== null) params.set(key, String(value));
    });
    return params.toString();
  };

  const chooseDialog = async (mode, prompt, defaultName = "") => {
    const result = asObject(await window.chooseDialog(queryFromObject({ mode, prompt, defaultName })));
    return result.ok ? result.value : "";
  };

  const cropString = () => {
    const x = Number(ui.cropX.value || 0);
    const y = Number(ui.cropY.value || 0);
    const w = Number(ui.cropW.value || 0);
    const h = Number(ui.cropH.value || 0);
    return w > 0 && h > 0 ? `${x} ${y} ${w} ${h}` : "";
  };

  const updateFisheyeAvailability = () => {
    const enabled = !!cropString();
    ui.fisheye.disabled = !enabled;
    if (!enabled) ui.fisheye.checked = false;
  };

  const setInputFilesList = (files) => {
    state.selectedInputFiles = files;
    if (!files.length) {
      ui.inputFilesList.textContent = "No files selected yet.";
      ui.inputFilesList.classList.add("empty");
      return;
    }
    ui.inputFilesList.classList.remove("empty");
    ui.inputFilesList.textContent = files.join("\n");
    const inferredDir = dirname(files[0]);
    if (inferredDir) {
      ui.workdir.value = inferredDir;
      if (!ui.outputDir.value || ui.outputDir.value === state.info?.defaultWorkdir) ui.outputDir.value = inferredDir;
      if (!ui.calWorkdir.value || ui.calWorkdir.value === state.info?.defaultWorkdir) ui.calWorkdir.value = inferredDir;
      if (!ui.shutterWorkdir.value || ui.shutterWorkdir.value === state.info?.defaultWorkdir) ui.shutterWorkdir.value = inferredDir;
      if (!ui.apertureWorkdir.value || ui.apertureWorkdir.value === state.info?.defaultWorkdir) ui.apertureWorkdir.value = inferredDir;
    }
    if (!ui.previewSource.value) {
      ui.previewSource.value = files[0];
      generatePreview(files[0]).catch((error) => {
        ui.luminanceSummary.textContent = String(error);
      });
    }
  };

  const setAllWorkdirs = (value) => {
    [ui.workdir, ui.outputDir, ui.calWorkdir, ui.shutterWorkdir, ui.apertureWorkdir].forEach((field) => {
      if (field) field.value = value;
    });
  };

  const activeWorkdir = () => (
    ui.workdir.value
    || dirname(state.selectedInputFiles[0] || "")
    || dirname(ui.previewSource.value || "")
    || ui.outputDir.value
    || state.info?.defaultWorkdir
    || ""
  );

  const parseRangeSummary = (text) => {
    const match = text.match(/using\s+\d+\s+frames,\s+range min:([^\s,]+),\s+max:([^\s]+)/i);
    if (!match) return "No luminance range yet.";
    return `Estimated luminance coverage: ${match[1]} to ${match[2]} cd/m²`;
  };

  const collectIssues = (text) => {
    const lines = text.split(/\n+/).filter(Boolean);
    const issues = lines.filter((line) => /warning:|underexpos|overexpos/i.test(line));
    return issues.join("\n");
  };

  const updateStatus = (progress, stage, status) => {
    const safe = clamp(progress, 0, 100);
    ui.progressFill.style.width = `${safe}%`;
    ui.progressPercent.textContent = `${Math.round(safe)}%`;
    ui.progressStage.textContent = stage || "waiting";
    ui.mergeStatus.textContent = status;
  };

  const stopPolling = () => {
    if (state.pollTimer) clearInterval(state.pollTimer);
    state.pollTimer = null;
  };

  const previewCropObject = () => ({
    x: Number(ui.cropX.value || 0),
    y: Number(ui.cropY.value || 0),
    w: Number(ui.cropW.value || 0),
    h: Number(ui.cropH.value || 0),
  });

  const setCrop = (x, y, w, h) => {
    ui.cropX.value = Math.max(0, Math.round(x));
    ui.cropY.value = Math.max(0, Math.round(y));
    ui.cropW.value = Math.max(0, Math.round(w));
    ui.cropH.value = Math.max(0, Math.round(h));
    updateFisheyeAvailability();
    renderViewer();
  };

  const updateViewerButtons = () => {
    ui.modeCrop.classList.toggle("active", state.viewer.mode === "crop");
    ui.modePan.classList.toggle("active", state.viewer.mode === "pan");
    ui.fitView.classList.toggle("active", state.viewer.preset === "fit");
    ui.actualPixels.classList.toggle("active", state.viewer.preset === "actual");
    ui.previewStage.classList.toggle("is-pan", state.viewer.mode === "pan");
    ui.previewStage.classList.toggle("dragging", !!state.viewer.drag);
    ui.viewerModeLabel.textContent = state.viewer.mode === "crop" ? "Crop mode" : "Pan mode";
  };

  const ensureCanvasSize = () => {
    const rect = ui.previewCanvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, Math.round(rect.width * dpr));
    const height = Math.max(1, Math.round(rect.height * dpr));
    if (ui.previewCanvas.width !== width || ui.previewCanvas.height !== height) {
      ui.previewCanvas.width = width;
      ui.previewCanvas.height = height;
    }
    return { width: rect.width, height: rect.height, dpr };
  };

  const currentLayout = () => {
    if (!state.preview || !state.viewer.processedCanvas) return null;
    const canvasState = ensureCanvasSize();
    const imageWidth = state.preview.previewWidth || state.viewer.processedCanvas.width;
    const imageHeight = state.preview.previewHeight || state.viewer.processedCanvas.height;
    if (!imageWidth || !imageHeight || !canvasState.width || !canvasState.height) return null;

    const fitScale = Math.min(canvasState.width / imageWidth, canvasState.height / imageHeight);
    const drawScale = fitScale * state.viewer.zoom;
    const drawWidth = imageWidth * drawScale;
    const drawHeight = imageHeight * drawScale;
    const originX = (canvasState.width - drawWidth) * 0.5 + state.viewer.panX;
    const originY = (canvasState.height - drawHeight) * 0.5 + state.viewer.panY;

    return {
      canvasWidth: canvasState.width,
      canvasHeight: canvasState.height,
      dpr: canvasState.dpr,
      imageWidth,
      imageHeight,
      fitScale,
      drawScale,
      drawWidth,
      drawHeight,
      originX,
      originY,
    };
  };

  const falseColorRamp = (value) => {
    const t = clamp(value, 0, 1);
    const stops = [
      [0.0, [10, 12, 30]],
      [0.15, [44, 76, 179]],
      [0.32, [0, 168, 235]],
      [0.5, [42, 204, 127]],
      [0.68, [246, 208, 70]],
      [0.84, [233, 99, 39]],
      [1.0, [255, 245, 240]],
    ];
    for (let i = 1; i < stops.length; i += 1) {
      if (t <= stops[i][0]) {
        const [t0, c0] = stops[i - 1];
        const [t1, c1] = stops[i];
        const mix = (t - t0) / (t1 - t0);
        return c0.map((channel, index) => Math.round(channel + (c1[index] - channel) * mix));
      }
    }
    return stops[stops.length - 1][1];
  };

  const rebuildProcessedPreview = () => {
    if (!state.viewer.sourceData) {
      state.viewer.processedCanvas = null;
      renderViewer();
      return;
    }

    const width = state.viewer.sourceData.width;
    const height = state.viewer.sourceData.height;
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const offscreen = canvas.getContext("2d");
    const imageData = offscreen.createImageData(width, height);
    const source = state.viewer.sourceData.data;
    const output = imageData.data;
    const exposureFactor = Math.pow(2, state.viewer.exposure);
    const gamma = clamp(state.viewer.gamma, 0.6, 2.4);

    for (let index = 0; index < source.length; index += 4) {
      let r = clamp((source[index] / 255) * exposureFactor, 0, 1);
      let g = clamp((source[index + 1] / 255) * exposureFactor, 0, 1);
      let b = clamp((source[index + 2] / 255) * exposureFactor, 0, 1);

      if (state.viewer.falseColor) {
        const y = clamp(0.265 * r + 0.67 * g + 0.065 * b, 0, 1);
        [r, g, b] = falseColorRamp(y).map((channel) => channel / 255);
      } else if (gamma !== 1) {
        const invGamma = 1 / gamma;
        r = Math.pow(r, invGamma);
        g = Math.pow(g, invGamma);
        b = Math.pow(b, invGamma);
      }

      output[index] = Math.round(clamp(r, 0, 1) * 255);
      output[index + 1] = Math.round(clamp(g, 0, 1) * 255);
      output[index + 2] = Math.round(clamp(b, 0, 1) * 255);
      output[index + 3] = 255;
    }

    offscreen.putImageData(imageData, 0, 0);
    state.viewer.processedCanvas = canvas;
    renderViewer();
  };

  const fitViewer = () => {
    state.viewer.zoom = 1;
    state.viewer.panX = 0;
    state.viewer.panY = 0;
    state.viewer.preset = "fit";
    updateViewerButtons();
    renderViewer();
  };

  const actualPixels = () => {
    const layout = currentLayout();
    if (!layout) return;
    state.viewer.zoom = 1 / layout.fitScale;
    state.viewer.panX = 0;
    state.viewer.panY = 0;
    state.viewer.preset = "actual";
    updateViewerButtons();
    renderViewer();
  };

  const sourceToCanvas = (sourceX, sourceY) => {
    const layout = currentLayout();
    if (!layout || !state.preview) return null;
    const previewX = (sourceX / state.preview.sourceWidth) * layout.imageWidth;
    const previewY = (sourceY / state.preview.sourceHeight) * layout.imageHeight;
    return {
      x: layout.originX + previewX * layout.drawScale,
      y: layout.originY + previewY * layout.drawScale,
    };
  };

  const canvasToSource = (clientX, clientY) => {
    const layout = currentLayout();
    if (!layout || !state.preview) return null;
    const rect = ui.previewCanvas.getBoundingClientRect();
    const x = clientX - rect.left;
    const y = clientY - rect.top;
    const previewX = (x - layout.originX) / layout.drawScale;
    const previewY = (y - layout.originY) / layout.drawScale;
    if (previewX < 0 || previewY < 0 || previewX > layout.imageWidth || previewY > layout.imageHeight)
      return null;
    return {
      x: (previewX / layout.imageWidth) * state.preview.sourceWidth,
      y: (previewY / layout.imageHeight) * state.preview.sourceHeight,
      previewX,
      previewY,
    };
  };

  const updateReadout = (point) => {
    if (!point || !state.viewer.sourceData || !state.preview) {
      ui.viewerReadout.textContent = "Move over the image to inspect preview pixels.";
      return;
    }

    const x = clamp(Math.round(point.x), 0, Math.max(0, state.preview.sourceWidth - 1));
    const y = clamp(Math.round(point.y), 0, Math.max(0, state.preview.sourceHeight - 1));
    const px = clamp(Math.round((x / state.preview.sourceWidth) * (state.viewer.sourceData.width - 1)), 0, state.viewer.sourceData.width - 1);
    const py = clamp(Math.round((y / state.preview.sourceHeight) * (state.viewer.sourceData.height - 1)), 0, state.viewer.sourceData.height - 1);
    const index = (py * state.viewer.sourceData.width + px) * 4;
    const data = state.viewer.sourceData.data;
    ui.viewerReadout.textContent = `x ${x}  y ${y}  ·  preview RGB ${data[index]}, ${data[index + 1]}, ${data[index + 2]}`;
  };

  const renderViewer = () => {
    const canvasState = ensureCanvasSize();
    ctx.save();
    ctx.setTransform(canvasState.dpr, 0, 0, canvasState.dpr, 0, 0);
    ctx.clearRect(0, 0, canvasState.width, canvasState.height);

    const layout = currentLayout();
    updateViewerButtons();
    if (!layout || !state.viewer.processedCanvas) {
      ui.viewerEmpty.classList.remove("hidden");
      ui.viewerZoomLabel.textContent = "Fit";
      updateReadout(null);
      ctx.restore();
      return;
    }

    ui.viewerEmpty.classList.add("hidden");
    ctx.drawImage(
      state.viewer.processedCanvas,
      layout.originX,
      layout.originY,
      layout.drawWidth,
      layout.drawHeight,
    );

    const crop = previewCropObject();
    if (crop.w > 0 && crop.h > 0) {
      const topLeft = sourceToCanvas(crop.x, crop.y);
      const bottomRight = sourceToCanvas(crop.x + crop.w, crop.y + crop.h);
      if (topLeft && bottomRight) {
        const x = topLeft.x;
        const y = topLeft.y;
        const w = bottomRight.x - topLeft.x;
        const h = bottomRight.y - topLeft.y;
        ctx.fillStyle = "rgba(10, 10, 10, 0.28)";
        ctx.fillRect(0, 0, canvasState.width, y);
        ctx.fillRect(0, y, x, h);
        ctx.fillRect(x + w, y, canvasState.width - (x + w), h);
        ctx.fillRect(0, y + h, canvasState.width, canvasState.height - (y + h));
        ctx.strokeStyle = "#f29a6d";
        ctx.lineWidth = 2;
        ctx.strokeRect(x, y, w, h);
      }
    }

    ui.viewerZoomLabel.textContent = `${Math.round(layout.drawScale * 100)}%`;
    updateReadout(state.viewer.hover);
    ctx.restore();
  };

  const setPreviewResult = async (result, label) => {
    state.preview = result;
    state.viewer.hover = null;

    await new Promise((resolve, reject) => {
      ui.previewImage.onload = () => resolve();
      ui.previewImage.onerror = () => reject(new Error("Could not load preview image."));
      ui.previewImage.src = `${result.previewUrl}?t=${Date.now()}`;
    });

    const scratch = document.createElement("canvas");
    scratch.width = ui.previewImage.naturalWidth;
    scratch.height = ui.previewImage.naturalHeight;
    const scratchCtx = scratch.getContext("2d");
    scratchCtx.drawImage(ui.previewImage, 0, 0);
    state.viewer.sourceData = scratchCtx.getImageData(0, 0, scratch.width, scratch.height);
    state.preview.previewWidth = result.previewWidth || ui.previewImage.naturalWidth;
    state.preview.previewHeight = result.previewHeight || ui.previewImage.naturalHeight;

    ui.previewMeta.textContent = label || `${result.sourceWidth} x ${result.sourceHeight}`;
    fitViewer();
    rebuildProcessedPreview();
  };

  const generatePreview = async (sourceOverride = "") => {
    const source = sourceOverride || ui.previewSource.value;
    if (!source) return;
    const result = asObject(await window.makePreview(queryFromObject({
      workdir: activeWorkdir(),
      source,
      fitWidth: 2200,
      fitHeight: 1500,
    })));
    await setPreviewResult(result, `${result.sourceWidth} x ${result.sourceHeight}`);
  };

  const refreshAppInfo = async () => {
    const info = asObject(await window.appInfo());
    state.info = info;
    state.detectedPaths.mergehdr = info.mergehdrPath || "";
    state.detectedPaths.tev = info.tevPath || "";
    state.detectedPaths.oiiotool = info.oiiotoolPath || "";
    setAllWorkdirs(info.defaultWorkdir || "");
    ui.settingsMergehdrPath.value = info.savedMergehdrPath || info.mergehdrPath || "";
    ui.settingsTevPath.value = info.savedTevPath || info.tevPath || "";
    ui.settingsOiiotoolPath.value = info.savedOiiotoolPath || info.oiiotoolPath || "";
    ui.settingsProfilesInfo.textContent = (info.profiles || []).join("\n") || "(no profiles found)";

    ui.profile.innerHTML = "";
    const autoOption = document.createElement("option");
    autoOption.value = "";
    autoOption.textContent = "Auto from metadata";
    ui.profile.appendChild(autoOption);
    (info.profiles || []).forEach((name) => {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      ui.profile.appendChild(option);
    });
  };

  const loadProfileDefaults = async () => {
    if (!ui.profile.value) {
      state.profileDefaults = {};
      ui.blackLevel.placeholder = "Auto from metadata";
      ui.whiteLevel.placeholder = "Auto from metadata";
      ui.saturation.placeholder = "Auto default";
      updateFisheyeAvailability();
      return;
    }

    const info = asObject(await window.profileInfo(queryFromObject({ profile: ui.profile.value })));
    if (!info.ok) return;
    state.profileDefaults = info;

    if (info.crop) {
      const parts = info.crop.trim().split(/\s+/);
      if (parts.length === 4) {
        ui.cropX.value = parts[0];
        ui.cropY.value = parts[1];
        ui.cropW.value = parts[2];
        ui.cropH.value = parts[3];
      }
    }
    if (info.colorspace) ui.colorspace.value = info.colorspace;
    updateFisheyeAvailability();
    if (info.fisheye && !ui.fisheye.disabled) ui.fisheye.checked = info.fisheye.toLowerCase() === "true";
    ui.blackLevel.placeholder = info.black ? `Preset ${info.black}` : "Auto from metadata";
    ui.whiteLevel.placeholder = info.white ? `Preset ${info.white}` : "Auto from metadata";
    ui.saturation.placeholder = info.saturation ? `Preset ${info.saturation}` : "Auto default";
    renderViewer();
  };

  const startPolling = (jobId) => {
    stopPolling();
    state.currentJobId = jobId;
    state.pollTimer = setInterval(async () => {
      const result = asObject(await window.pollMergeJob(queryFromObject({ jobId })));
      ui.commandOut.textContent = result.command || "(no command)";
      ui.logOut.textContent = result.log || "(no output yet)";
      ui.luminanceSummary.textContent = parseRangeSummary(result.log || "");
      const issues = collectIssues(result.log || "");
      if (issues) {
        ui.issuesBox.textContent = issues;
        ui.issuesBox.classList.remove("hidden");
      } else {
        ui.issuesBox.textContent = "";
        ui.issuesBox.classList.add("hidden");
      }
      updateStatus(
        result.progress || 0,
        result.stage || "running",
        result.finished ? (result.exitCode === 0 ? "Finished" : "Failed") : "Running",
      );
      state.latestOutput = result.output || "";
      if (result.finished) {
        stopPolling();
        if (result.exitCode === 0 && state.latestOutput) {
          try {
            ui.previewSource.value = state.latestOutput;
            const preview = asObject(await window.makePreview(queryFromObject({
              workdir: activeWorkdir(),
              source: state.latestOutput,
              fitWidth: 2200,
              fitHeight: 1500,
            })));
            await setPreviewResult(preview, `Merged output: ${preview.sourceWidth} x ${preview.sourceHeight}`);
          } catch (_) {
          }
        }
      }
    }, 500);
  };

  const detectCells = async (which) => {
    const image = which === "reference" ? ui.calReference.value : ui.calTest.value;
    if (!image) return;
    const result = asObject(await window.runChartCells(queryFromObject({
      workdir: ui.calWorkdir.value,
      image,
      patches: 24,
      cols: 6,
      rows: 4,
      inset: 0.15,
      rowMajor: 0,
    })));
    if (!result.ok) {
      ui.calibrationResult.textContent = result.log || "Chart detection failed.";
      return;
    }
    if (which === "reference") ui.calRefCells.value = result.cellsPath;
    else ui.calTestCells.value = result.cellsPath;
    state.latestChart = result;
    ui.chartMeta.textContent = result.cellsPath || "Cells detected";
    ui.chartCellsText.textContent = result.cellsText || "(no cells)";
    ui.chartPreviewImage.src = `${result.previewUrl}?t=${Date.now()}`;
    ui.chartPreviewImage.parentElement.classList.add("has-image");
  };

  const updateRecommendedMatrix = (text) => {
    const match = text.match(/Color Matrix SLSQP Minimization \([^)]+\):\s+([^\n]+)/)
      || text.match(/Color Matrix Optimization:\s+([^\n]+)/);
    state.recommendedXyzcam = match ? match[1].trim() : "";
    ui.recommendedMatrix.textContent = state.recommendedXyzcam ? "Recommended xyzcam ready" : "No recommendation yet";
  };

  const beginViewerDrag = (event) => {
    if (event.button !== 0) return;
    if (!state.preview || !state.viewer.processedCanvas) return;
    const point = canvasToSource(event.clientX, event.clientY);
    if (!point && state.viewer.mode !== "pan") return;

    if (state.viewer.mode === "pan") {
      state.viewer.drag = {
        type: "pan",
        startX: event.clientX,
        startY: event.clientY,
        panX: state.viewer.panX,
        panY: state.viewer.panY,
      };
    } else {
      state.viewer.drag = {
        type: "crop",
        startX: point.x,
        startY: point.y,
      };
      setCrop(point.x, point.y, 0, 0);
    }
    updateViewerButtons();
  };

  const handleViewerMove = (event) => {
    if (!state.viewer.drag && !ui.previewCanvas.contains(event.target)) {
      return;
    }

    if (state.viewer.drag && state.viewer.drag.type === "pan") {
      state.viewer.panX = state.viewer.drag.panX + (event.clientX - state.viewer.drag.startX);
      state.viewer.panY = state.viewer.drag.panY + (event.clientY - state.viewer.drag.startY);
      state.viewer.preset = "manual";
      renderViewer();
      return;
    }

    const point = canvasToSource(event.clientX, event.clientY);
    state.viewer.hover = point;

    if (state.viewer.drag && state.viewer.drag.type === "crop" && point) {
      const x1 = Math.min(state.viewer.drag.startX, point.x);
      const y1 = Math.min(state.viewer.drag.startY, point.y);
      const x2 = Math.max(state.viewer.drag.startX, point.x);
      const y2 = Math.max(state.viewer.drag.startY, point.y);
      setCrop(x1, y1, x2 - x1, y2 - y1);
      return;
    }

    renderViewer();
  };

  const endViewerDrag = () => {
    if (!state.viewer.drag) return;
    state.viewer.drag = null;
    updateViewerButtons();
    renderViewer();
  };

  const handleViewerWheel = (event) => {
    if (!state.preview || !state.viewer.processedCanvas) return;
    event.preventDefault();
    const layout = currentLayout();
    if (!layout) return;

    const rect = ui.previewCanvas.getBoundingClientRect();
    const mouseX = event.clientX - rect.left;
    const mouseY = event.clientY - rect.top;
    const previewX = (mouseX - layout.originX) / layout.drawScale;
    const previewY = (mouseY - layout.originY) / layout.drawScale;
    const zoomFactor = event.deltaY < 0 ? 1.1 : 1 / 1.1;
    state.viewer.zoom = clamp(state.viewer.zoom * zoomFactor, 0.25, 24);
    state.viewer.preset = "manual";

    const newLayout = currentLayout();
    if (newLayout) {
      state.viewer.panX = mouseX - (newLayout.canvasWidth - newLayout.imageWidth * newLayout.drawScale) * 0.5 - previewX * newLayout.drawScale;
      state.viewer.panY = mouseY - (newLayout.canvasHeight - newLayout.imageHeight * newLayout.drawScale) * 0.5 - previewY * newLayout.drawScale;
    }
    renderViewer();
  };

  ui.navItems.forEach((item) => item.addEventListener("click", () => setPanel(item.dataset.panel)));
  ui.toggleColorInfo.addEventListener("click", () => ui.colorInfo.classList.toggle("hidden"));

  [ui.cropX, ui.cropY, ui.cropW, ui.cropH].forEach((field) => field.addEventListener("input", () => {
    updateFisheyeAvailability();
    renderViewer();
  }));

  ui.fitView.addEventListener("click", fitViewer);
  ui.actualPixels.addEventListener("click", actualPixels);
  ui.modeCrop.addEventListener("click", () => {
    state.viewer.mode = "crop";
    updateViewerButtons();
  });
  ui.modePan.addEventListener("click", () => {
    state.viewer.mode = "pan";
    updateViewerButtons();
  });
  ui.viewerExposure.addEventListener("input", () => {
    state.viewer.exposure = Number(ui.viewerExposure.value || 0);
    rebuildProcessedPreview();
  });
  ui.viewerGamma.addEventListener("input", () => {
    state.viewer.gamma = Number(ui.viewerGamma.value || 1);
    rebuildProcessedPreview();
  });
  ui.viewerFalseColor.addEventListener("change", () => {
    state.viewer.falseColor = ui.viewerFalseColor.checked;
    rebuildProcessedPreview();
  });

  ui.previewCanvas.addEventListener("mousedown", beginViewerDrag);
  ui.previewCanvas.addEventListener("mousemove", handleViewerMove);
  ui.previewCanvas.addEventListener("mouseleave", () => {
    state.viewer.hover = null;
    if (!state.viewer.drag) renderViewer();
  });
  ui.previewCanvas.addEventListener("wheel", handleViewerWheel, { passive: false });
  window.addEventListener("mousemove", handleViewerMove);
  window.addEventListener("mouseup", endViewerDrag);
  window.addEventListener("resize", renderViewer);

  if (ui.pickWorkdir) {
    ui.pickWorkdir.addEventListener("click", async () => {
      const folder = await chooseDialog("folder", "Choose working folder");
      if (folder) setAllWorkdirs(folder);
    });
  }

  ui.pickInputFiles.addEventListener("click", async () => {
    const files = await chooseDialog("files", "Choose RAW files");
    if (!files) return;
    setInputFilesList(files.split(/\n+/).filter(Boolean));
  });

  ui.pickOutputDir.addEventListener("click", async () => {
    const folder = await chooseDialog("folder", "Choose output folder");
    if (folder) ui.outputDir.value = folder;
  });

  ui.pickPreviewSource.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose preview source");
    if (!file) return;
    ui.previewSource.value = file;
    const inferredDir = dirname(file);
    if (inferredDir) {
      if (!ui.workdir.value || ui.workdir.value === state.info?.defaultWorkdir) ui.workdir.value = inferredDir;
      if (!ui.outputDir.value || ui.outputDir.value === state.info?.defaultWorkdir) ui.outputDir.value = inferredDir;
    }
    await generatePreview(file);
  });

  ui.generatePreview.addEventListener("click", async () => {
    try {
      await generatePreview();
    } catch (error) {
      ui.luminanceSummary.textContent = String(error);
    }
  });

  ui.clearCrop.addEventListener("click", () => {
    setCrop(0, 0, 0, 0);
  });

  ui.outputFormat.addEventListener("change", () => {
    const ext = ui.outputFormat.value === "exr" ? ".exr" : ".hdr";
    ui.outputName.value = ui.outputName.value.replace(/\.(hdr|exr)$/i, "") + ext;
  });

  ui.profile.addEventListener("change", loadProfileDefaults);

  ui.startMerge.addEventListener("click", async () => {
    ui.startMerge.disabled = true;
    updateStatus(0, "starting", "Starting");
    ui.issuesBox.classList.add("hidden");
    try {
      const result = asObject(await window.startMerge(queryFromObject({
        workdir: activeWorkdir(),
        inputs: ui.inputPattern.value,
        inputFiles: state.selectedInputFiles.join("\n"),
        outputDir: ui.outputDir.value,
        outputName: ui.outputName.value,
        outputFormat: ui.outputFormat.value,
        profile: ui.profile.value,
        colorspace: ui.colorspace.value,
        black: ui.blackLevel.value,
        white: ui.whiteLevel.value,
        saturation: ui.saturation.value,
        crop: cropString(),
        demosaic: ui.demosaic.value,
        mergeWeight: ui.mergeWeight.value,
        rawgrid: ui.rawgrid.checked ? 1 : 0,
        bloom: ui.bloom.checked ? 1 : 0,
        fisheye: ui.fisheye.checked ? 1 : 0,
        xyzcam: ui.xyzcamOverride.value,
        extra: ui.extra.value,
      })));
      if (result.ok) {
        ui.commandOut.textContent = result.command;
        startPolling(result.jobId);
      } else {
        ui.logOut.textContent = result.error || "Could not start merge.";
      }
    } catch (error) {
      ui.logOut.textContent = String(error);
    } finally {
      ui.startMerge.disabled = false;
    }
  });

  ui.openTev.addEventListener("click", async () => {
    const target = state.latestOutput
      || ui.previewSource.value
      || [ui.outputDir.value, ui.outputName.value].filter(Boolean).join("/");
    const result = asObject(await window.openTev(target));
    if (!result.ok) ui.logOut.textContent = result.error || "Could not show the file in the native viewer.";
  });

  ui.copyCommand.addEventListener("click", async () => {
    const text = ui.commandOut.textContent.trim();
    if (!text || text === "(not run yet)") return;
    try {
      await navigator.clipboard.writeText(text);
    } catch (_) {
    }
  });

  ui.pickCalReference.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose reference image or TSV");
    if (file) ui.calReference.value = file;
  });

  ui.pickCalTest.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose test image or TSV");
    if (file) ui.calTest.value = file;
  });

  ui.detectRefCells.addEventListener("click", async () => detectCells("reference"));
  ui.detectTestCells.addEventListener("click", async () => detectCells("test"));

  ui.runColorCalibrate.addEventListener("click", async () => {
    ui.runColorCalibrate.disabled = true;
    ui.calibrationResult.textContent = "(running calibration)";
    try {
      const result = asObject(await window.runColorCalibrate(queryFromObject({
        workdir: ui.calWorkdir.value,
        reference: ui.calReference.value,
        test: ui.calTest.value,
        refCells: ui.calRefCells.value,
        testCells: ui.calTestCells.value,
        refcol: ui.calRefcol.value,
        xyzcam: ui.calXyzcam.value,
        minimizer: ui.calMinimizer.value,
        verbose: ui.calVerbose.checked ? 1 : 0,
      })));
      ui.calibrationResult.textContent = result.result || "(no output)";
      updateRecommendedMatrix(result.result || "");
    } catch (error) {
      ui.calibrationResult.textContent = String(error);
    } finally {
      ui.runColorCalibrate.disabled = false;
    }
  });

  ui.copyRecommendedXyzcam.addEventListener("click", async () => {
    if (!state.recommendedXyzcam) return;
    ui.xyzcamOverride.value = state.recommendedXyzcam;
    ui.calXyzcam.value = state.recommendedXyzcam;
    try {
      await navigator.clipboard.writeText(state.recommendedXyzcam);
    } catch (_) {
    }
  });

  ui.useMergeCropForShutter.addEventListener("click", () => {
    const crop = cropString();
    if (crop) ui.shutterCrops.value = crop;
  });

  ui.useMergeCropForAperture.addEventListener("click", () => {
    const crop = cropString();
    if (crop) ui.apertureCrop.value = crop;
  });

  ui.runShutter.addEventListener("click", async () => {
    ui.runShutter.disabled = true;
    ui.shutterResult.textContent = "(running shutter calibration)";
    try {
      const result = asObject(await window.runShutter(queryFromObject({
        workdir: ui.shutterWorkdir.value,
        sequences: ui.shutterSequences.value,
        crops: ui.shutterCrops.value,
        channel: ui.shutterChannel.value,
        dataout: ui.shutterDataout.value,
      })));
      ui.shutterResult.textContent = result.result || "(no output)";
    } catch (error) {
      ui.shutterResult.textContent = String(error);
    } finally {
      ui.runShutter.disabled = false;
    }
  });

  ui.runAperture.addEventListener("click", async () => {
    ui.runAperture.disabled = true;
    ui.apertureResult.textContent = "(running aperture calibration)";
    try {
      const result = asObject(await window.runAperture(queryFromObject({
        workdir: ui.apertureWorkdir.value,
        sequences: ui.apertureSequences.value,
        crop: ui.apertureCrop.value,
        shutterc: ui.apertureShutterc.value,
      })));
      ui.apertureResult.textContent = result.result || "(no output)";
    } catch (error) {
      ui.apertureResult.textContent = String(error);
    } finally {
      ui.runAperture.disabled = false;
    }
  });

  ui.pickSettingsMergehdr.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose mergehdr binary");
    if (file) ui.settingsMergehdrPath.value = file;
  });

  ui.pickSettingsTev.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose tev binary");
    if (file) ui.settingsTevPath.value = file;
  });

  ui.pickSettingsOiiotool.addEventListener("click", async () => {
    const file = await chooseDialog("file", "Choose oiiotool binary");
    if (file) ui.settingsOiiotoolPath.value = file;
  });

  ui.saveSettings.addEventListener("click", async () => {
    await window.saveSettings(queryFromObject({
      mergehdrPath: ui.settingsMergehdrPath.value,
      tevPath: ui.settingsTevPath.value,
      oiiotoolPath: ui.settingsOiiotoolPath.value,
    }));
    await refreshAppInfo();
  });

  ui.useDetectedPaths.addEventListener("click", () => {
    ui.settingsMergehdrPath.value = state.detectedPaths.mergehdr || "";
    ui.settingsTevPath.value = state.detectedPaths.tev || "";
    ui.settingsOiiotoolPath.value = state.detectedPaths.oiiotool || "";
  });

  refreshAppInfo().then(() => {
    updateFisheyeAvailability();
    updateViewerButtons();
    renderViewer();
  });
});
