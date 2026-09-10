/*
 * Copyright (c) 2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "api/imgui_compat.h"

extern "C" RIMGUI_EXPORT void RIMGUI_CALL
remixapi_imgui_RegisterDrawCallback(PFN_remixapi_imgui_DrawCallback callback, void *userData) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_UnregisterDrawCallback(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Begin(const char *name, int *p_open, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_End(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginChild(const char *str_id, float w, float h,
                                                                   int border, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndChild(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowPos(float x, float y, int cond) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowSize(float w, float h, int cond) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowCollapsed(int collapsed, int cond) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetNextWindowFocus(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetWindowSize(float *out_w, float *out_h) {
  if (out_w) {
    *out_w = 0.0f;
  }
  if (out_h) {
    *out_h = 0.0f;
  }
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetWindowPos(float *out_x, float *out_y) {
  if (out_x) {
    *out_x = 0.0f;
  }
  if (out_y) {
    *out_y = 0.0f;
  }
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SameLine(float offset_from_start_x, float spacing) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Separator(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Spacing(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Dummy(float w, float h) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Indent(float indent_w) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Unindent(float indent_w) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_NewLine(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BeginGroup(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndGroup(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_GetContentRegionAvail(float *out_w, float *out_h) {
  if (out_w) {
    *out_w = 0.0f;
  }
  if (out_h) {
    *out_h = 0.0f;
  }
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushID_Str(const char *str_id) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushID_Int(int int_id) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopID(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_Text(const char *text) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TextColored(float r, float g, float b, float a,
                                                                     const char *text) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TextWrapped(const char *text) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BulletText(const char *text) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_LabelText(const char *label, const char *text) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Button(const char *label, float w, float h) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SmallButton(const char *label) { return 0; }

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Checkbox(const char *label, int *v) { return 0; }

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_RadioButton(const char *label, int active) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat(const char *label, float *v, float v_min,
                                                                    float v_max, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderInt(const char *label, int *v, int v_min,
                                                                  int v_max, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat2(const char *label, float *v, float v_min,
                                                                     float v_max, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_SliderFloat3(const char *label, float *v, float v_min,
                                                                     float v_max, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_DragFloat(const char *label, float *v, float v_speed,
                                                                  float v_min, float v_max,
                                                                  const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_DragInt(const char *label, int *v, float v_speed,
                                                                int v_min, int v_max, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputFloat(const char *label, float *v, float step,
                                                                   float step_fast, const char *format) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputInt(const char *label, int *v, int step,
                                                                 int step_fast) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_InputText(const char *label, char *buf,
                                                                  uint32_t buf_size, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorEdit3(const char *label, float *col) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorEdit4(const char *label, float *col, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_ColorPicker3(const char *label, float *col) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginCombo(const char *label,
                                                                   const char *preview_value, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndCombo(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_Selectable(const char *label, int selected, int flags,
                                                                   float w, float h) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_CollapsingHeader(const char *label, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TreeNode(const char *label) { return 0; }

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TreePop(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTabBar(const char *str_id, int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTabBar(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTabItem(const char *label, int *p_open,
                                                                     int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTabItem(void) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_BeginTable(const char *str_id, int column, int flags,
                                                                   float outer_w, float outer_h,
                                                                   float inner_width) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTable(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableNextRow(int row_flags, float min_row_height) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TableNextColumn(void) { return 0; }

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_TableSetColumnIndex(int column_n) { return 0; }

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableSetupColumn(const char *label, int flags,
                                                                          float init_width_or_weight,
                                                                          uint32_t user_id) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_TableHeadersRow(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_BeginTooltip(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_EndTooltip(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetTooltip(const char *text) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_ProgressBar(float fraction, float w, float h,
                                                                     const char *overlay) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_IsItemHovered(int flags) { return 0; }

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_IsItemClicked(int mouse_button) { return 0; }

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_SetItemDefaultFocus(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleColor(int idx, float r, float g, float b,
                                                                        float a) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopStyleColor(int count) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleVar_Float(int idx, float val) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PushStyleVar_Vec2(int idx, float x, float y) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PopStyleVar(int count) {}

extern "C" RIMGUI_EXPORT int RIMGUI_CALL remixapi_imgui_PlotBeginPlot(const char *title_id, float w, float h,
                                                                      int flags) {
  return 0;
}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotEndPlot(void) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotPlotLine(const char *label_id,
                                                                      const float *values, int count) {}

extern "C" RIMGUI_EXPORT void RIMGUI_CALL remixapi_imgui_PlotPlotBars(const char *label_id,
                                                                      const float *values, int count,
                                                                      double bar_size) {}

extern "C" RIMGUI_EXPORT int remixapi_imgui_HasDrawCallback() { return 0; }
extern "C" RIMGUI_EXPORT void remixapi_imgui_InvokeDrawCallback() {}
