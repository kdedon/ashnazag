/*
 * w16ftmod.h -- FreeType's modules for the Win16 environment, in place
 * of FreeType's own ftmodule.h: the TrueType driver, the sfnt tables it
 * reads and the black-and-white rasterizer.
 */
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
