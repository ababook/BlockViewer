/*
 *
 *      Shrink code block.
 *      Edit by Jinhao
 *      2017.3.3
 *
 */

#include <dbg.hpp>
#include <hexrays.hpp>

 // Hex-Rays API pointer
hexdsp_t *hexdsp = NULL;
cexpr_t *hint = NULL;
static bool inited = false;

struct Block
{
	ea_t addr;
	cblock_t *block;
	ctype_t type;
	cinsn_t *insn;
};

struct Pseudo
{
	vdui_t *vu;
	qvector<Block> bVec;
	Pseudo(vdui_t *v):vu(v){}
	void Collapse(cinsn_t *i)
	{
		switch (i->op)
		{
		case cit_block:
		case cit_for:
		case cit_while:
		case cit_do:
		case cit_switch:
		case cit_asm:
			{
				bVec.push_back({ i->ea, i->cblock, i->op, i });
				i->op = cit_expr;
				i->cexpr = hint;				
			}
			break;
		case cit_expr:
			{
				if (i->cexpr != hint)
				{
					return;
				}
				for (auto iter = bVec.begin(); iter != bVec.end(); ++iter)
				{
					if (iter->addr == i->ea)
					{
						i->op = iter->type;
						i->cblock = iter->block;
						bVec.erase(iter);
						break;
					}
				}
			}
			break;
		default:
			break;
		}
		vu->refresh_ctext();
	}
	void ExpandAll()
	{
		for (auto iter = bVec.begin(); iter != bVec.end(); ++iter)
		{
			iter->insn->op = iter->type;
			iter->insn->cblock = iter->block;
		}
		bVec.clear();
		vu->refresh_ctext();
	}
};

qvector<Pseudo *> pseuVec;
Pseudo * GetPseudo(vdui_t *vu)
{
	for (auto iter = pseuVec.begin(); iter != pseuVec.end(); ++iter)
	{
		if ((*iter)->vu==vu)
		{
			return *iter;
		}
	}
	Pseudo * newone = new Pseudo(vu);
	pseuVec.push_back(newone);
	return newone;
}

struct ida_local block_finder_t : public ctree_visitor_t
{
	ea_t ea;
	cinsn_t *found;
	block_finder_t(ea_t e) : ctree_visitor_t(CV_FAST | CV_INSNS), ea(e) {}
	int idaapi visit_insn(cinsn_t *i)
	{
		if (i->ea == ea)
		{
			found = i;
			return 1; // stop enumeration
		}
		return 0;
	}
};

bool hasWord(
#ifdef __IDA7__
    TWidget *v
#else
    TCustomControl *v
#endif
)
{
	// query the cursor position
	int x, y;
	if (get_custom_viewer_place(v, false, &x, &y) == NULL)
		return false;

	// query the line at the cursor
#ifdef __IDA7__
    qstring buf;
    const char *line = get_custom_viewer_curline(v, false);
    tag_remove(&buf, line);
    if (x >= buf.length())
        return false;
#else
    char buf[MAXSTR];
    const char *line = get_custom_viewer_curline(v, false);
    tag_remove(line, buf, MAXSTR);
    if (x >= (int)strlen(buf))
        return false;
#endif	

	return true;
}

//--------------------------------------------------------------------------
// This callback handles various hexrays events.
static int idaapi callback(void *, hexrays_event_t event, va_list va)
{
	switch (event)
	{
	case hxe_double_click:
		{
			vdui_t *vu = va_arg(va, vdui_t *);
			if (!hasWord(vu->ct))
			{
				if (!vu->in_ctree())
				{
					if (vu->locked())
					{
						vu->collapse_lvars(false);
						vu->set_locked(false);
					}
					else
					{
						vu->collapse_lvars(true);
						vu->set_locked(true);
					}
					return 1;
				}
				else
				{
					block_finder_t bf(vu->head.loc.ea);
					if (bf.apply_to(&vu->cfunc->body, NULL))
					{
				 		GetPseudo(vu)->Collapse(bf.found);
						return 1;
					}
				}
			}
		}
	break;

	case hxe_switch_pseudocode:
		{
			vdui_t *vu = va_arg(va, vdui_t *);
			GetPseudo(vu)->ExpandAll();
		}
		break;
	case hxe_close_pseudocode:
		{
			vdui_t *vu = va_arg(va, vdui_t *);
			Pseudo *pse = GetPseudo(vu);
			pse->ExpandAll();
			pseuVec.del(pse);
			delete pse;
		}
		break;
	case hxe_flowchart:
		{
#ifdef __IDA7__
            vdui_t *vu = get_widget_vdui(get_current_viewer());
#else
            vdui_t *vu = get_tform_vdui(get_current_tform());
#endif
			
			if (vu)
			{
				GetPseudo(vu)->ExpandAll();
			}			
		}
		break;

	default:
		break;
	}
	return 0;
}

static ssize_t idaapi ExpandAllBeforeChange(void *, int code, va_list va)
{
#ifdef __IDA7__
    vdui_t *vu = get_widget_vdui(get_current_viewer());
#else
    vdui_t *vu = get_tform_vdui(get_current_tform());
#endif
	if (vu &&(code==idb_event::renaming_struc_member || code == idb_event::changing_struc_member ||
		code == idb_event::changing_cmt || code == idb_event::changing_op_ti))
	{
		GetPseudo(vu)->ExpandAll();
	}
	return 0;
}

#ifndef __IDA7__
static ssize_t idaapi DisableBeforeDebug(void *, int code, va_list va)
{
    if (code == dbg_process_start || code == dbg_process_attach)
    {
        remove_hexrays_callback(callback, NULL);
        unhook_from_notification_point(HT_IDB, ExpandAllBeforeChange, NULL);
    }
    else if (code == dbg_process_exit || code == dbg_process_detach)
    {
        install_hexrays_callback(callback, NULL);
        hook_to_notification_point(HT_IDB, ExpandAllBeforeChange, NULL);
    }
    return 0;
}
#endif


//--------------------------------------------------------------------------
// Initialize the plugin.
int idaapi init(void)
{
	if (!init_hexrays_plugin())
		return PLUGIN_SKIP; // no decompiler

	install_hexrays_callback(callback, NULL);
	hook_to_notification_point(HT_IDB, ExpandAllBeforeChange, NULL);

#ifndef __IDA7__
    hook_to_notification_point(HT_DBG, DisableBeforeDebug, NULL);
#endif
    
	hint = new cexpr_t(cot_str, NULL);
	hint->string = "{...} // double click right area to expand.";
	inited = true;

	msg("BlockViewer plugin by jhftss loaded.\n");
	return PLUGIN_KEEP;
}

//--------------------------------------------------------------------------
void idaapi term(void)
{
	if (inited)
	{
		hint->string = NULL;
		delete hint;
		for (auto iter = pseuVec.begin(); iter != pseuVec.end(); ++iter)
		{
			delete *iter;
		}
		pseuVec.clear();

#ifndef __IDA7__
        unhook_from_notification_point(HT_DBG, DisableBeforeDebug, NULL);
#endif

		remove_hexrays_callback(callback, NULL);
		unhook_from_notification_point(HT_IDB, ExpandAllBeforeChange, NULL);
		term_hexrays_plugin();
	}
}

//--------------------------------------------------------------------------
#ifdef __IDA7__
bool idaapi run(size_t)
#else
void idaapi run(int)
#endif
{
	func_t *pfn = get_func(get_screen_ea());
    if (pfn)
    {
#ifdef __IDA7__
        int code = ask_buttons("~S~tart", "~E~nd", "~C~ancel", -1, "Jump to function start or end ?");
        switch (code)
        {
        case -1:    // cancel
            break;
        case 0:     // end
            jumpto(pfn->end_ea);
            break;
        case 1:     // start
            jumpto(pfn->start_ea);
            break;
        }
#else
        int code = askbuttons_c("~S~tart", "~E~nd", "~C~ancel", -1, "Jump to function start or end ?");
        switch (code)
        {
        case -1:    // cancel
            break;
        case 0:     // end
            jumpto(pfn->endEA);
            break;
        case 1:     // start
            jumpto(pfn->startEA);
            break;
        }
#endif        
    }

#ifdef __IDA7__
    return true;
#endif	
}

plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  0,          // plugin flags
  init,                 // initialize
  term,                 // terminate. this pointer may be NULL.
  run,                  // invoke plugin
  "",              // long comment about the plugin
						// it could appear in the status line
						// or as a hint
  "",                   // multiline help about the plugin
  "CodeViewer", // the preferred short name of the plugin
  "J"                    // the preferred hotkey to run the plugin
};
