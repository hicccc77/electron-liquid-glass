{
  "targets": [
    {
      "target_name": "liquid_glass",
      "conditions": [
        [
          "OS=='win'",
          {
            "sources": [
              "src/addon.cc",
              "src/capture.cc",
              "src/d3d_utils.cc",
              "src/panel.cc",
              "src/renderer.cc",
              "src/session.cc"
            ],
            "libraries": [
              "d3d11.lib",
              "dxgi.lib",
              "dcomp.lib",
              "d3dcompiler.lib",
              "user32.lib"
            ]
          },
          {
            "sources": [
              "src/addon_stub.cc"
            ]
          }
        ]
      ],
      "include_dirs": [
        "<!(node -p \"require('node-addon-api').include_dir\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "NAPI_VERSION=8",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        "UNICODE",
        "_UNICODE"
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "AdditionalOptions": [
            "/std:c++17",
            "/utf-8"
          ]
        }
      },
      "xcode_settings": {
        "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
        "MACOSX_DEPLOYMENT_TARGET": "10.15"
      },
      "cflags_cc": [
        "-std=c++17"
      ]
    }
  ]
}
