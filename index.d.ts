/**
 * electron-liquid-glass
 * Windows 原生低延迟液态玻璃背景面板（DXGI Desktop Duplication + D3D11 + DirectComposition）
 *
 * 所有坐标与尺寸均为屏幕物理像素。
 */

/** 玻璃视觉参数（长度单位：物理像素） */
export interface GlassEffectParams {
    /** 圆角半径，默认 20 */
    cornerRadius?: number
    /** 高斯模糊σ，默认 5 */
    blurSigma?: number
    /** 边缘位移强度（70 = 标准强度），默认 70 */
    displacementScale?: number
    /** RGB 色散强度（0~3），默认 2 */
    aberrationIntensity?: number
    /** 饱和度增益，默认 1.4 */
    saturation?: number
    /**
     * 采样区相对面板的偏移。常规玻璃保持 0；
     * 非 0 时面板折射别处内容（测量/特效场景）
     */
    sourceOffsetX?: number
    sourceOffsetY?: number
}

/** 亮度采样带（矩形为面板本地物理像素） */
export interface LumaBand {
    id: number
    x: number
    y: number
    width: number
    height: number
}

/** 亮度带统计：玻璃模糊层的均值色 + luma p15/p85（0~255） */
export interface LumaBandStats {
    r: number
    g: number
    b: number
    darkTail: number
    lightTail: number
}

export interface CreatePanelOptions extends GlassEffectParams {
    x: number
    y: number
    width: number
    height: number
    /** 显示器缩放系数（内部几何常量按此缩放），默认 1 */
    dpr?: number
    /**
     * 从屏幕捕获（截屏/录屏/DDA/WGC）中排除面板，默认 true。
     * 必须保持 true 才能避免自采集反馈回路；仅测试可关。
     */
    excludeFromCapture?: boolean
    /** 把面板 z 序钉在该窗口正下方（Electron BrowserWindow 或 HWND Buffer） */
    anchorWindow?: unknown
    /** 亮度采样带（配合 onLuma 做自适应文字反色） */
    lumaBands?: LumaBand[]
    /**
     * 亮度回调：bands 为 { [bandId]: LumaBandStats }。
     * 重绘驱动：面板下方内容变化的同一帧推送（上限 ~60Hz），桌面静止时不推送
     */
    onLuma?: (bands: Record<string, LumaBandStats>) => void
}

export interface GlassPanel {
    readonly id: number
    /** 显示并淡入（默认 120ms） */
    show(fadeMs?: number): void
    /** 淡出并隐藏（默认 100ms；0 = 立即） */
    hide(fadeMs?: number): void
    destroy(): void
    setBounds(bounds: { x: number; y: number; width: number; height: number }): void
    /** 更新视觉参数（传完整参数对象，缺省字段回落默认值） */
    setParams(params: GlassEffectParams): void
    anchor(windowOrHwnd: unknown): void
    setLumaBands(bands: LumaBand[]): void
    onLuma(cb: (bands: Record<string, LumaBandStats>) => void): void
}

/** 当前环境是否支持原生玻璃（Windows 10 2004+ 且原生二进制已构建） */
export function isSupported(): boolean

/** 创建玻璃面板；原生不可用时返回 null（调用方应回退） */
export function createPanel(options: CreatePanelOptions): GlassPanel | null

/** 停止工作线程并销毁所有面板（进程退出前自动调用） */
export function shutdown(): void
