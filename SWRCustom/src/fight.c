#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "mud.h"

extern char lastplayercmd[MAX_INPUT_LENGTH];
extern CHAR_DATA *gch_prev;

/* From Skills.c */
int ris_save( CHAR_DATA * ch, int chance, int ris );
void write_ship_list( void );

/*
 * Local functions.
 */
static void dam_message( CHAR_DATA * ch, CHAR_DATA * victim, int dam,
    int dt );
static int align_compute( const CHAR_DATA * gch, const CHAR_DATA * victim );
ch_ret one_hit( CHAR_DATA * ch, CHAR_DATA * victim, int dt );
static int obj_hitroll( OBJ_DATA * obj );
static bool get_cover( CHAR_DATA * ch );
static bool dual_flip = FALSE;

/*
 * Check to see if weapon is poisoned.
 */
bool is_wielding_poisoned( const CHAR_DATA * ch )
{
  OBJ_DATA *obj;

  if( ( obj = get_eq_char( ch, WEAR_WIELD ) )
      && ( IS_SET( obj->extra_flags, ITEM_POISONED ) ) )
    return TRUE;

  return FALSE;
}

/*
 * hunting, hating and fearing code				-Thoric
 */
bool is_hunting( const CHAR_DATA * ch, const CHAR_DATA * victim )
{
  return ch->hunting && ch->hunting->who == victim;
}

bool is_hating( const CHAR_DATA * ch, const CHAR_DATA * victim )
{
  return ch->hating && ch->hating->who == victim;
}

bool is_fearing( const CHAR_DATA * ch, const CHAR_DATA * victim )
{
  return ch->fearing && ch->fearing->who == victim;
}

void stop_hunting( CHAR_DATA * ch )
{
  if( ch->hunting )
  {
    STRFREE( ch->hunting->name );
    DISPOSE( ch->hunting );
    ch->hunting = NULL;
  }
  return;
}

void stop_hating( CHAR_DATA * ch )
{
  if( ch->hating )
  {
    STRFREE( ch->hating->name );
    DISPOSE( ch->hating );
    ch->hating = NULL;
  }
  return;
}

void stop_fearing( CHAR_DATA * ch )
{
  if( ch->fearing )
  {
    STRFREE( ch->fearing->name );
    DISPOSE( ch->fearing );
    ch->fearing = NULL;
  }
  return;
}

void start_hunting( CHAR_DATA * ch, CHAR_DATA * victim )
{
  if( ch->hunting )
    stop_hunting( ch );

  CREATE( ch->hunting, HHF_DATA, 1 );
  ch->hunting->name = QUICKLINK( victim->name );
  ch->hunting->who = victim;
  return;
}

void start_hating( CHAR_DATA * ch, CHAR_DATA * victim )
{
  if( ch->hating )
    stop_hating( ch );

  CREATE( ch->hating, HHF_DATA, 1 );
  ch->hating->name = QUICKLINK( victim->name );
  ch->hating->who = victim;
  return;
}

void start_fearing( CHAR_DATA * ch, CHAR_DATA * victim )
{
  if( ch->fearing )
    stop_fearing( ch );

  CREATE( ch->fearing, HHF_DATA, 1 );
  ch->fearing->name = QUICKLINK( victim->name );
  ch->fearing->who = victim;
  return;
}


int max_fight( const CHAR_DATA * ch )
{
  return 8;
}

/*
 * Control the fights going on.
 * Called periodically by update_handler.
 * Many hours spent fixing bugs in here by Thoric, as noted by residual
 * debugging checks.  If you never get any of these error messages again
 * in your logs... then you can comment out some of the checks without
 * worry.
 */
void violence_update( void )
{
  char buf[MAX_STRING_LENGTH];
  CHAR_DATA *ch = NULL;
  CHAR_DATA *lst_ch = NULL;
  CHAR_DATA *victim = NULL;
  CHAR_DATA *rch = NULL, *rch_next = NULL;
  AFFECT_DATA *paf = NULL, *paf_next = NULL;
  TIMER *timer = NULL, *timer_next = NULL;
  ch_ret retcode = rNONE;
  SKILLTYPE *skill = NULL;

  for( ch = last_char; ch; lst_ch = ch, ch = gch_prev )
  {
    set_cur_char( ch );

    if( ch == first_char && ch->prev )
    {
      bug( "ERROR: first_char->prev != NULL, fixing...", 0 );
      ch->prev = NULL;
    }

    gch_prev = ch->prev;

    if( gch_prev && gch_prev->next != ch )
    {
      sprintf( buf,
	  "FATAL: violence_update: %s->prev->next doesn't point to ch.",
	  ch->name );
      bug( buf, 0 );
      bug( "Short-cutting here", 0 );
      ch->prev = NULL;
      gch_prev = NULL;
    }

    /*
     * See if we got a pointer to someone who recently died...
     * if so, either the pointer is bad... or it's a player who
     * "died", and is back at the healer...
     * Since he/she's in the char_list, it's likely to be the later...
     * and should not already be in another fight already
     */
    if( char_died( ch ) )
      continue;

    /*
     * See if we got a pointer to some bad looking data...
     */
    if( !ch->in_room || !ch->name )
    {
      log_string( "violence_update: bad ch record!  (Shortcutting.)" );
      sprintf( buf,
	  "ch: %ld  ch->in_room: %ld  ch->prev: %ld  ch->next: %ld",
	  ( long ) ch, ( long ) ch->in_room, ( long ) ch->prev,
	  ( long ) ch->next );
      log_string( buf );
      log_string( lastplayercmd );
      if( lst_ch )
	sprintf( buf, "lst_ch: %ld  lst_ch->prev: %ld  lst_ch->next: %ld",
	    ( long ) lst_ch, ( long ) lst_ch->prev,
	    ( long ) lst_ch->next );
      else
	strcpy( buf, "lst_ch: NULL" );
      log_string( buf );
      gch_prev = NULL;
      continue;
    }

    /*
     * Experience gained during battle deceases as battle drags on
     */
    if( ch->fighting )
      if( ( ++ch->fighting->duration % 24 ) == 0 )
	ch->fighting->xp = ( ( ch->fighting->xp * 9 ) / 10 );


    for( timer = ch->first_timer; timer; timer = timer_next )
    {
      timer_next = timer->next;
      if( --timer->count <= 0 )
      {
	if( timer->type == TIMER_DO_FUN )
	{
	  int tempsub;

	  tempsub = ch->substate;
	  ch->substate = timer->value;
	  ( timer->do_fun ) ( ch, STRLIT_EMPTY );
	  if( char_died( ch ) )
	    break;
	  ch->substate = tempsub;
	}
	extract_timer( ch, timer );
      }
    }

    if( char_died( ch ) )
      continue;

    /*
     * We need spells that have shorter durations than an hour.
     * So a melee round sounds good to me... -Thoric
     */
    for( paf = ch->first_affect; paf; paf = paf_next )
    {
      paf_next = paf->next;
      if( paf->duration > 0 )
	paf->duration--;
      else if( paf->duration < 0 )
	;
      else
      {
	if( !paf_next
	    || paf_next->type != paf->type || paf_next->duration > 0 )
	{
	  skill = get_skilltype( paf->type );
	  if( paf->type > 0 && skill && skill->msg_off )
	  {
	    set_char_color( AT_WEAROFF, ch );
	    send_to_char( skill->msg_off, ch );
	    send_to_char( "\r\n", ch );
	  }
	}
	if( paf->type == gsn_possess )
	{
	  ch->desc->character = ch->desc->original;
	  ch->desc->original = NULL;
	  ch->desc->character->desc = ch->desc;
	  ch->desc->character->switched = NULL;
	  ch->desc = NULL;
	}
	affect_remove( ch, paf );
      }
    }

    if( ( victim = who_fighting( ch ) ) == NULL
	|| IS_AFFECTED( ch, AFF_PARALYSIS ) )
      continue;

    retcode = rNONE;

    if( IS_SET( ch->in_room->room_flags, ROOM_SAFE ) )
    {
      sprintf( buf, "violence_update: %s fighting %s in a SAFE room.",
	  ch->name, victim->name );
      log_string( buf );
      stop_fighting( ch, TRUE );
    }
    else if( IS_AWAKE( ch ) && ch->in_room == victim->in_room )
      retcode = multi_hit( ch, victim, TYPE_UNDEFINED );
    else
      stop_fighting( ch, FALSE );

    if( char_died( ch ) )
      continue;

    if( retcode == rCHAR_DIED || ( victim = who_fighting( ch ) ) == NULL )
      continue;

    /*
     *  Mob triggers
     */
    rprog_rfight_trigger( ch );
    if( char_died( ch ) )
      continue;
    mprog_hitprcnt_trigger( ch, victim );
    if( char_died( ch ) )
      continue;
    mprog_fight_trigger( ch, victim );
    if( char_died( ch ) )
      continue;

    /*
     * Fun for the whole family!
     */
    for( rch = ch->in_room->first_person; rch; rch = rch_next )
    {
      rch_next = rch->next_in_room;

      if( IS_AWAKE( rch ) && !rch->fighting )
      {
	/*
	 * PC's auto-assist others in their group.
	 */
	if( !IS_NPC( ch ) || IS_AFFECTED( ch, AFF_CHARM ) )
	{
	  if( ( !IS_NPC( rch ) || IS_AFFECTED( rch, AFF_CHARM ) )
	      && is_same_group( ch, rch ) )
	    multi_hit( rch, victim, TYPE_UNDEFINED );
	  continue;
	}

	/*
	 * NPC's assist NPC's of same type or if citizen.
	 */
	if( IS_NPC( rch ) && !IS_AFFECTED( rch, AFF_CHARM )
	    && !IS_SET( rch->act, ACT_NOASSIST ) )
	{
	  if( char_died( ch ) )
	    break;
	  if( rch->pIndexData == ch->pIndexData
	      || ( IS_NPC( ch ) && IS_SET( ch->act, ACT_CITIZEN ) ) )
	  {
	    CHAR_DATA *vch;
	    CHAR_DATA *target;
	    int number;

	    target = NULL;
	    number = 0;
	    for( vch = ch->in_room->first_person; vch;
		vch = vch->next_in_room )
	    {
	      if( can_see( rch, vch )
		  && is_same_group( vch, victim )
		  && number_range( 0, number ) == 0 )
	      {
		target = vch;
		number++;
	      }
	    }

	    if( target )
	      multi_hit( rch, target, TYPE_UNDEFINED );
	  }
	}
      }
    }
  }

  return;
}



/*
 * Do one group of attacks.
 */
ch_ret multi_hit( CHAR_DATA * ch, CHAR_DATA * victim, int dt )
{
  int chance = 0;
  int dual_bonus = 0;
  ch_ret retcode = rNONE;

  /* add timer if player is attacking another player */
  if( !IS_NPC( ch ) && !IS_NPC( victim ) )
    add_timer( ch, TIMER_RECENTFIGHT, 20, NULL, 0 );

  if( !IS_NPC( ch ) && IS_SET( ch->act, PLR_NICE ) && !IS_NPC( victim ) )
    return rNONE;

  if( ( retcode = one_hit( ch, victim, dt ) ) != rNONE )
    return retcode;

  if( who_fighting( ch ) != victim || dt == gsn_backstab || dt == gsn_circle )
    return rNONE;

  /* Very high chance of hitting compared to chance of going berserk */
  /* 40% or higher is always hit.. don't learn anything here though. */
  /* -- Altrag */
  chance =
    IS_NPC( ch ) ? 100 : character_skill_level( ch, gsn_berserk ) * 5 / 2;
  if( IS_AFFECTED( ch, AFF_BERSERK ) && number_percent() < chance )
    if( ( retcode = one_hit( ch, victim, dt ) ) != rNONE ||
	who_fighting( ch ) != victim )
      return retcode;

  if( get_eq_char( ch, WEAR_DUAL_WIELD ) )
  {
    dual_bonus = character_skill_level( ch, gsn_dual_wield ) / 10;
    chance = character_skill_level( ch, gsn_dual_wield );

    if( number_percent() < chance )
    {
      learn_from_success( ch, gsn_dual_wield );
      retcode = one_hit( ch, victim, dt );
      if( retcode != rNONE || who_fighting( ch ) != victim )
	return retcode;
    }
    else
      learn_from_failure( ch, gsn_dual_wield );
  }
  else
    dual_bonus = 0;

  if( ch->move < 10 )
    dual_bonus = -20;

  /*
   * NPC predetermined number of attacks                      -Thoric
   */
  if( IS_NPC( ch ) && ch->numattacks > 0 )
  {
    for( chance = 0; chance < ch->numattacks; chance++ )
    {
      retcode = one_hit( ch, victim, dt );

      if( retcode != rNONE || who_fighting( ch ) != victim )
	return retcode;
    }

    return retcode;
  }

  chance =
    IS_NPC( ch ) ? 0 : ( character_skill_level( ch, gsn_second_attack ) +
	( int ) ( dual_bonus / 1.5 ) );

  if( number_percent() < chance )
  {
    learn_from_success( ch, gsn_second_attack );
    retcode = one_hit( ch, victim, dt );
    if( retcode != rNONE || who_fighting( ch ) != victim )
      return retcode;
  }
  else
    learn_from_failure( ch, gsn_second_attack );

  chance =
    IS_NPC( ch ) ? 0 : ( character_skill_level( ch, gsn_third_attack ) +
	( int ) ( dual_bonus * 1.5 ) ) / 2;
  if( number_percent() < chance )
  {
    learn_from_success( ch, gsn_third_attack );
    retcode = one_hit( ch, victim, dt );
    if( retcode != rNONE || who_fighting( ch ) != victim )
      return retcode;
  }
  else
    learn_from_failure( ch, gsn_third_attack );

  retcode = rNONE;
  /*
     chance = IS_NPC(ch) ? (int) (ch->top_level / 4) : 0;
     if ( number_percent( ) < chance )
     retcode = one_hit( ch, victim, dt );
     */
  if( retcode == rNONE )
  {
    int move;

    if( !IS_AFFECTED( ch, AFF_FLYING ) && !IS_AFFECTED( ch, AFF_FLOATING ) )
      move =
	encumbrance( ch,
	    movement_loss[UMIN
	    ( SECT_MAX - 1,
	      ch->in_room->sector_type )] );
    else
      move = encumbrance( ch, 1 );
    if( ch->move )
      ch->move = UMAX( 0, ch->move - move );
  }

  return retcode;
}


/*
 * Weapon types, haus
 */
int weapon_prof_bonus_check( CHAR_DATA * ch, OBJ_DATA * wield, int *gsn_ptr )
{
  int bonus = 0;;

  *gsn_ptr = -1;

  if( !IS_NPC( ch ) && wield )
  {
    switch ( wield->value[3] )
    {
      default:		/* WEAPON_NONE or other invalid */
	*gsn_ptr = -1;
	break;

      case WEAPON_LIGHTSABER:
	*gsn_ptr = gsn_lightsabers;
	break;

      case WEAPON_VIBRO_BLADE:
	*gsn_ptr = gsn_vibro_blades;
	break;

      case 4:
	*gsn_ptr = gsn_flexible_arms;
	break;

      case 5:
	*gsn_ptr = gsn_talonous_arms;
	break;

      case WEAPON_BLASTER:
	*gsn_ptr = gsn_blasters;
	break;

      case 8:
	*gsn_ptr = gsn_bludgeons;
	break;

      case 9:
	*gsn_ptr = gsn_bowcasters;
	break;

      case 11:
	*gsn_ptr = gsn_force_pikes;
	break;

    }

    if( *gsn_ptr != -1 )
      bonus = character_skill_level( ch, *gsn_ptr );
  }

  if( IS_NPC( ch ) && wield )
    bonus = get_trust( ch );

  return bonus;
}

/*
 * Calculate the tohit bonus on the object and return RIS values.
 * -- Altrag
 */
int obj_hitroll( OBJ_DATA * obj )
{
  int tohit = 0;
  AFFECT_DATA *paf;

  for( paf = obj->pIndexData->first_affect; paf; paf = paf->next )
    if( paf->location == APPLY_HITROLL )
      tohit += paf->modifier;
  for( paf = obj->first_affect; paf; paf = paf->next )
    if( paf->location == APPLY_HITROLL )
      tohit += paf->modifier;
  return tohit;
}

/*
 * Offensive shield level modifier
 */
short off_shld_lvl( CHAR_DATA * ch, CHAR_DATA * victim )
{
  return 0;
}

/*
 * Hit one guy once.
 */
ch_ret one_hit( CHAR_DATA * ch, CHAR_DATA * victim, int dt )
{
  OBJ_DATA *wield = NULL;
  int victim_ac = 0;
  int thac0 = 0;
  int plusris = 0;
  int dam = 0, x = 0;
  int diceroll = 0;
  int attacktype = 0, cnt = 0;
  int prof_bonus = 0;
  int prof_gsn = 0;
  ch_ret retcode = rNONE;
  int chance = 0;
  bool fail = FALSE;
  AFFECT_DATA af;


  /*
   * Can't beat a dead char!
   * Guard against weird room-leavings.
   */
  if( victim->position == POS_DEAD || ch->in_room != victim->in_room )
    return rVICT_DIED;


  /*
   * Figure out the weapon doing the damage                   -Thoric
   */
  if( ( wield = get_eq_char( ch, WEAR_DUAL_WIELD ) ) != NULL )
  {
    if( dual_flip == FALSE )
    {
      dual_flip = TRUE;
      wield = get_eq_char( ch, WEAR_WIELD );
    }
    else
      dual_flip = FALSE;
  }
  else
    wield = get_eq_char( ch, WEAR_WIELD );

  prof_bonus = weapon_prof_bonus_check( ch, wield, &prof_gsn );

  if( ch->fighting		/* make sure fight is already started */
      && dt == TYPE_UNDEFINED && IS_NPC( ch ) && ch->attacks != 0 )
  {
    cnt = 0;
    for( ;; )
    {
      x = number_range( 0, 6 );
      attacktype = 1 << x;
      if( IS_SET( ch->attacks, attacktype ) )
	break;
      if( cnt++ > 16 )
      {
	attacktype = 0;
	break;
      }
    }
    if( attacktype == ATCK_BACKSTAB )
      attacktype = 0;
    if( wield && number_percent() > 25 )
      attacktype = 0;
    switch ( attacktype )
    {
      default:
	break;
      case ATCK_KICK:
	do_kick( ch, STRLIT_EMPTY );
	retcode = global_retcode;
	break;
      case ATCK_TRIP:
	attacktype = 0;
	break;
    }
    if( attacktype )
      return retcode;
  }

  if( dt == TYPE_UNDEFINED )
  {
    dt = TYPE_HIT;
    if( wield && wield->item_type == ITEM_WEAPON )
      dt += wield->value[3];
  }

  /*
   * Calculate to-hit-armor-class-0 versus armor.
   */
  thac0 = 20 - GET_HITROLL( ch );
  victim_ac = ( int ) ( GET_AC( victim ) / 10 );

  /* if you can't see what's coming... */
  if( wield && !can_see_obj( victim, wield ) )
    victim_ac += 1;
  if( !can_see( ch, victim ) )
    victim_ac -= 4;

  if( !IS_AWAKE( victim ) )
    victim_ac += 5;

  /* Weapon proficiency bonus */
  victim_ac += prof_bonus / 20;

  /*
   * The moment of excitement!
   */
  diceroll = number_range( 1, 20 );

  if( diceroll == 1 || ( diceroll < 20 && diceroll < thac0 - victim_ac ) )
  {
    /* Miss. */
    if( prof_gsn != -1 )
      learn_from_failure( ch, prof_gsn );
    damage( ch, victim, 0, dt );
    return rNONE;
  }

  /*
   * Hit.
   * Calc damage.
   */

  if( !wield )			/* dice formula fixed by Thoric */
    dam = number_range( ch->barenumdie, ch->baresizedie * ch->barenumdie );
  else
    dam = number_range( wield->value[1], wield->value[2] );

  /*
   * Bonuses.
   */

  dam += GET_DAMROLL( ch );

  if( prof_bonus )
    dam *= ( 1 + prof_bonus / 100 );


  if( !IS_NPC( ch ) && character_skill_level( ch, gsn_enhanced_damage ) > 0 )
  {
    dam +=
      ( int ) ( dam * character_skill_level( ch, gsn_enhanced_damage ) /
		120 );
    learn_from_success( ch, gsn_enhanced_damage );
  }

  if( !IS_AWAKE( victim ) )
    dam *= 2;
  if( dt == gsn_backstab )
    dam *= 3;

  if( dt == gsn_circle )
    dam *= 2;

  plusris = 0;

  if( wield )
  {
    if( IS_SET( wield->extra_flags, ITEM_MAGIC ) )
      dam = ris_damage( victim, dam, RIS_MAGIC );
    else
      dam = ris_damage( victim, dam, RIS_NONMAGIC );

    /*
     * Handle PLUS1 - PLUS6 ris bits vs. weapon hitroll       -Thoric
     */
    plusris = obj_hitroll( wield );
  }
  else
    dam = ris_damage( victim, dam, RIS_NONMAGIC );

  /* check for RIS_PLUSx                                      -Thoric */
  if( dam )
  {
    int flag, res, imm, sus, mod;

    if( plusris )
      plusris = RIS_PLUS1 << UMIN( plusris, 7 );

    /* initialize values to handle a zero plusris */
    imm = res = -1;
    sus = 1;

    /* find high ris */
    for( flag = RIS_PLUS1; flag <= RIS_PLUS6; flag <<= 1 )
    {
      if( IS_SET( victim->immune, flag ) )
	imm = flag;
      if( IS_SET( victim->resistant, flag ) )
	res = flag;
      if( IS_SET( victim->susceptible, flag ) )
	sus = flag;
    }
    mod = 10;
    if( imm >= plusris )
      mod -= 10;
    if( res >= plusris )
      mod -= 2;
    if( sus <= plusris )
      mod += 2;

    /* check if immune */
    if( mod <= 0 )
      dam = -1;
    if( mod != 10 )
      dam = ( dam * mod ) / 10;
  }


  /* PCs take less damage now that they only have 100 hitpoints to start */

  if( !IS_NPC( victim ) )
  {
    if( get_curr_con( victim ) > 20 )
      dam = ( int ) ( dam * 0.1 );
    else if( get_curr_con( victim ) > 18 )
      dam = ( int ) ( dam * 0.15 );
    else if( get_curr_con( victim ) > 15 )
      dam = ( int ) ( dam * 0.2 );
    else if( get_curr_con( victim ) > 15 )
      dam = ( int ) ( dam * 0.25 );
    else
      dam = ( int ) ( dam * 0.3 );
  }

  /* 
   * check to see if weapon is charged 
   */

  if( dt == ( TYPE_HIT + WEAPON_BLASTER ) && wield
      && wield->item_type == ITEM_WEAPON )
  {
    if( wield->value[4] < 1 )
    {
      act( AT_YELLOW,
	  "$n points their blaster at you but nothing happens.", ch,
	  NULL, victim, TO_VICT );
      act( AT_YELLOW,
	  "*CLICK* ... your blaster needs a new ammunition cell!", ch,
	  NULL, victim, TO_CHAR );
      if( IS_NPC( ch ) )
      {
	do_remove( ch, wield->name );
      }
      return rNONE;
    }
    else if( wield->blaster_setting == BLASTER_FULL
	&& wield->value[4] >= 5 )
    {
      dam = ( int ) ( dam * 1.5 );
      wield->value[4] -= 5;
    }
    else if( wield->blaster_setting == BLASTER_HIGH
	&& wield->value[4] >= 4 )
    {
      dam = ( int ) ( dam * 1.25 );
      wield->value[4] -= 4;
    }
    else if( wield->blaster_setting == BLASTER_NORMAL
	&& wield->value[4] >= 3 )
    {
      wield->value[4] -= 3;
    }
    else if( wield->blaster_setting == BLASTER_STUN
	&& wield->value[4] >= 5 )
    {
      dam = 1;
      wield->value[4] -= 3;
      fail = FALSE;
      if( victim->was_stunned > 0
	  && !IS_AFFECTED( victim, AFF_PARALYSIS ) )
      {
	fail = TRUE;
	victim->was_stunned--;
      }
      chance = con_app[get_curr_con( ch )].stun;
      if( !fail && number_percent() < chance )
      {
	WAIT_STATE( victim, PULSE_VIOLENCE );
	act( AT_BLUE,
	    "Blue rings of energy from $N's blaster knock you down leaving you stunned!",
	    victim, NULL, ch, TO_CHAR );
	act( AT_BLUE,
	    "Blue rings of energy from your blaster strike $N, leaving $M stunned!",
	    ch, NULL, victim, TO_CHAR );
	act( AT_BLUE,
	    "Blue rings of energy from $n's blaster hit $N, leaving $M stunned!",
	    ch, NULL, victim, TO_NOTVICT );
	stop_fighting( victim, TRUE );
	if( !IS_AFFECTED( victim, AFF_PARALYSIS ) )
	{
	  af.type = gsn_stun;
	  af.location = APPLY_AC;
	  af.modifier = 100;
	  af.duration = 7;
	  af.bitvector = AFF_PARALYSIS;
	  affect_to_char( victim, &af );
	  update_pos( victim );
	  if( IS_NPC( victim ) )
	  {
	    start_hating( victim, ch );
	    start_hunting( victim, ch );
	    victim->was_stunned = 10;
	  }
	}
      }
      else
      {
	act( AT_BLUE,
	    "Blue rings of energy from $N's blaster hit you but have little effect",
	    victim, NULL, ch, TO_CHAR );
	act( AT_BLUE,
	    "Blue rings of energy from your blaster hit $N, but nothing seems to happen!",
	    ch, NULL, victim, TO_CHAR );
	act( AT_BLUE,
	    "Blue rings of energy from $n's blaster hit $N, but nothing seems to happen!",
	    ch, NULL, victim, TO_NOTVICT );

      }
    }
    else if( wield->blaster_setting == BLASTER_HALF
	&& wield->value[4] >= 2 )
    {
      dam = ( int ) ( dam * 0.75 );
      wield->value[4] -= 2;
    }
    else
    {
      dam = ( int ) ( dam * 0.5 );
      wield->value[4] -= 1;
    }

  }
  else if( dt == ( TYPE_HIT + WEAPON_VIBRO_BLADE )
      && wield && wield->item_type == ITEM_WEAPON )
  {
    if( wield->value[4] < 1 )
    {
      act( AT_YELLOW, "Your vibro-blade needs recharging ...", ch, NULL,
	  victim, TO_CHAR );
      dam /= 3;
    }
  }
  else if( dt == ( TYPE_HIT + WEAPON_LIGHTSABER )
      && wield && wield->item_type == ITEM_WEAPON )
  {
    if( wield->value[4] < 1 )
    {
      act( AT_YELLOW, "$n waves a dead hand grip around in the air.", ch,
	  NULL, victim, TO_VICT );
      act( AT_YELLOW,
	  "You need to recharge your lightsaber ... it seems to be lacking a blade.",
	  ch, NULL, victim, TO_CHAR );
      if( IS_NPC( ch ) )
      {
	do_remove( ch, wield->name );
      }
      return rNONE;
    }
  }

  if( dam <= 0 )
    dam = 1;

  if( prof_gsn != -1 )
  {
    if( dam > 0 )
      learn_from_success( ch, prof_gsn );
    else
      learn_from_failure( ch, prof_gsn );
  }

  /* immune to damage */
  if( dam == -1 )
  {
    if( dt >= 0 && dt < top_sn )
    {
      SKILLTYPE *skill = skill_table[dt];
      bool found = FALSE;

      if( skill->imm_char && skill->imm_char[0] != '\0' )
      {
	act( AT_HIT, skill->imm_char, ch, NULL, victim, TO_CHAR );
	found = TRUE;
      }
      if( skill->imm_vict && skill->imm_vict[0] != '\0' )
      {
	act( AT_HITME, skill->imm_vict, ch, NULL, victim, TO_VICT );
	found = TRUE;
      }
      if( skill->imm_room && skill->imm_room[0] != '\0' )
      {
	act( AT_ACTION, skill->imm_room, ch, NULL, victim, TO_NOTVICT );
	found = TRUE;
      }
      if( found )
	return rNONE;
    }
    dam = 0;
  }
  if( ( retcode = damage( ch, victim, dam, dt ) ) != rNONE )
    return retcode;
  if( char_died( ch ) )
    return rCHAR_DIED;
  if( char_died( victim ) )
    return rVICT_DIED;

  retcode = rNONE;
  if( dam == 0 )
    return retcode;

  /* weapon spells	-Thoric */
  if( wield && !IS_SET( victim->immune, RIS_MAGIC ) )
  {
    AFFECT_DATA *aff;

    for( aff = wield->pIndexData->first_affect; aff; aff = aff->next )
      if( aff->location == APPLY_WEAPONSPELL && IS_VALID_SN( aff->modifier )
	  && skill_table[aff->modifier]->spell_fun )
	retcode =
	  ( *skill_table[aff->modifier]->spell_fun ) ( aff->modifier, 100,
						       ch, victim );

    if( retcode == rSPELL_FAILED )
      retcode = rNONE;

    if( retcode != rNONE || char_died( ch ) || char_died( victim ) )
      return retcode;
    for( aff = wield->first_affect; aff; aff = aff->next )
      if( aff->location == APPLY_WEAPONSPELL && IS_VALID_SN( aff->modifier )
	  && skill_table[aff->modifier]->spell_fun )
	retcode =
	  ( *skill_table[aff->modifier]->spell_fun ) ( aff->modifier, 100,
						       ch, victim );

    if( retcode == rSPELL_FAILED )
      retcode = rNONE;

    if( retcode != rNONE || char_died( ch ) || char_died( victim ) )
      return retcode;
  }

  /*
   * magic shields that retaliate                             -Thoric
   */
  if( IS_AFFECTED( victim, AFF_FIRESHIELD )
      && !IS_AFFECTED( ch, AFF_FIRESHIELD ) )
    retcode =
      spell_fireball( gsn_fireball, off_shld_lvl( victim, ch ), victim, ch );
  if( retcode != rNONE || char_died( ch ) || char_died( victim ) )
    return retcode;

  if( retcode != rNONE || char_died( ch ) || char_died( victim ) )
    return retcode;

  if( IS_AFFECTED( victim, AFF_SHOCKSHIELD )
      && !IS_AFFECTED( ch, AFF_SHOCKSHIELD ) )
    retcode =
      spell_lightning_bolt( gsn_lightning_bolt, off_shld_lvl( victim, ch ),
	  victim, ch );
  if( retcode != rNONE || char_died( ch ) || char_died( victim ) )
    return retcode;

  /*
   *   folks with blasters move and snipe instead of getting neatin up in one spot.
   */
  if( IS_NPC( victim ) )
  {
    OBJ_DATA *weapon;

    weapon = get_eq_char( victim, WEAR_WIELD );
    if( weapon != NULL && weapon->value[3] == WEAPON_BLASTER
	&& get_cover( victim ) == TRUE )
    {
      if( IS_SET( victim->act, ACT_SENTINEL ) )
      {
	victim->was_sentinel = victim->in_room;
	REMOVE_BIT( victim->act, ACT_SENTINEL );
      }
      start_hating( victim, ch );
      start_hunting( victim, ch );
    }
  }

  return retcode;
}

/*
 * Calculate damage based on resistances, immunities and suceptibilities
 *					-Thoric
 */
short ris_damage( const CHAR_DATA * ch, short dam, int ris )
{
  short modifier = 10;

  if( IS_SET( ch->immune, ris ) )
    modifier -= 10;

  if( IS_SET( ch->resistant, ris ) )
    modifier -= 2;

  if( IS_SET( ch->susceptible, ris ) )
    modifier += 2;

  if( modifier <= 0 )
    return -1;

  if( modifier == 10 )
    return dam;

  return ( dam * modifier ) / 10;
}

/*
 * Inflict damage from a hit.
 */
ch_ret damage( CHAR_DATA * ch, CHAR_DATA * victim, int dam, int dt )
{
  char buf1[MAX_STRING_LENGTH];
  short dameq = 0;
  bool npcvict = FALSE;
  bool loot = FALSE;
  OBJ_DATA *damobj = NULL;
  ch_ret retcode = rNONE;
  int init_gold = 0, new_gold = 0, gold_diff = 0;

  if( !ch )
  {
    bug( "Damage: null ch!", 0 );
    return rERROR;
  }
  if( !victim )
  {
    bug( "Damage: null victim!", 0 );
    return rVICT_DIED;
  }

  if( victim->position == POS_DEAD )
    return rVICT_DIED;

  npcvict = IS_NPC( victim );

  /*
   * Check damage types for RIS                               -Thoric
   */
  if( dam && dt != TYPE_UNDEFINED )
  {
    if( IS_FIRE( dt ) )
      dam = ris_damage( victim, dam, RIS_FIRE );
    else if( IS_COLD( dt ) )
      dam = ris_damage( victim, dam, RIS_COLD );
    else if( IS_ACID( dt ) )
      dam = ris_damage( victim, dam, RIS_ACID );
    else if( IS_ELECTRICITY( dt ) )
      dam = ris_damage( victim, dam, RIS_ELECTRICITY );
    else if( IS_ENERGY( dt ) )
      dam = ris_damage( victim, dam, RIS_ENERGY );
    else if( IS_DRAIN( dt ) )
      dam = ris_damage( victim, dam, RIS_DRAIN );
    else if( dt == gsn_poison || IS_POISON( dt ) )
      dam = ris_damage( victim, dam, RIS_POISON );
    else if( dt == ( TYPE_HIT + 7 ) || dt == ( TYPE_HIT + 8 ) )
      dam = ris_damage( victim, dam, RIS_BLUNT );
    else
      if( dt == ( TYPE_HIT + 2 ) || dt == ( TYPE_HIT + 11 )
	  || dt == ( TYPE_HIT + 10 ) )
	dam = ris_damage( victim, dam, RIS_PIERCE );
      else
	if( dt == ( TYPE_HIT + 1 ) || dt == ( TYPE_HIT + 3 )
	    || dt == ( TYPE_HIT + 4 ) || dt == ( TYPE_HIT + 5 ) )
	  dam = ris_damage( victim, dam, RIS_SLASH );

    if( dam == -1 )
    {
      if( dt >= 0 && dt < top_sn )
      {
	bool found = FALSE;
	SKILLTYPE *skill = skill_table[dt];

	if( skill->imm_char && skill->imm_char[0] != '\0' )
	{
	  act( AT_HIT, skill->imm_char, ch, NULL, victim, TO_CHAR );
	  found = TRUE;
	}
	if( skill->imm_vict && skill->imm_vict[0] != '\0' )
	{
	  act( AT_HITME, skill->imm_vict, ch, NULL, victim, TO_VICT );
	  found = TRUE;
	}
	if( skill->imm_room && skill->imm_room[0] != '\0' )
	{
	  act( AT_ACTION, skill->imm_room, ch, NULL, victim,
	      TO_NOTVICT );
	  found = TRUE;
	}
	if( found )
	  return rNONE;
      }
      dam = 0;
    }
  }

  if( dam && npcvict && ch != victim )
  {
    if( !IS_SET( victim->act, ACT_SENTINEL ) )
    {
      if( victim->hunting )
      {
	if( victim->hunting->who != ch )
	{
	  STRFREE( victim->hunting->name );
	  victim->hunting->name = QUICKLINK( ch->name );
	  victim->hunting->who = ch;
	}
      }
      else
	start_hunting( victim, ch );
    }

    if( victim->hating )
    {
      if( victim->hating->who != ch )
      {
	STRFREE( victim->hating->name );
	victim->hating->name = QUICKLINK( ch->name );
	victim->hating->who = ch;
      }
    }
    else
      start_hating( victim, ch );
  }

  if( victim != ch )
  {
    /*
     * Certain attacks are forbidden.
     * Most other attacks are returned.
     */
    if( is_safe( ch, victim ) )
      return rNONE;


    if( victim->position > POS_STUNNED )
    {
      if( !victim->fighting )
	set_fighting( victim, ch );
      if( victim->fighting )
	victim->position = POS_FIGHTING;
    }

    if( victim->position > POS_STUNNED )
    {
      if( !ch->fighting )
	set_fighting( ch, victim );

      /*
       * If victim is charmed, ch might attack victim's master.
       */
      if( IS_NPC( ch )
	  && npcvict
	  && IS_AFFECTED( victim, AFF_CHARM )
	  && victim->master
	  && victim->master->in_room == ch->in_room
	  && number_bits( 3 ) == 0 )
      {
	stop_fighting( ch, FALSE );
	retcode = multi_hit( ch, victim->master, TYPE_UNDEFINED );
	return retcode;
      }
    }

    /*
     * More charm stuff.
     */
    if( victim->master == ch )
      stop_follower( victim );

    /*
     * Inviso attacks ... not.
     */
    if( IS_AFFECTED( ch, AFF_INVISIBLE ) )
    {
      affect_strip( ch, gsn_invis );
      affect_strip( ch, gsn_mass_invis );
      REMOVE_BIT( ch->affected_by, AFF_INVISIBLE );
      act( AT_MAGIC, "$n fades into existence.", ch, NULL, NULL,
	  TO_ROOM );
    }

    /* Take away Hide */
    if( IS_AFFECTED( ch, AFF_HIDE ) )
      REMOVE_BIT( ch->affected_by, AFF_HIDE );
    /*
     * Damage modifiers.
     */
    if( IS_AFFECTED( victim, AFF_SANCTUARY ) )
      dam /= 2;

    if( IS_AFFECTED( victim, AFF_PROTECT ) && IS_EVIL( ch ) )
      dam -= ( int ) ( dam / 4 );

    if( dam < 0 )
      dam = 0;

    /*
     * Check for disarm, trip, parry, and dodge.
     */
    if( dt >= TYPE_HIT )
    {
      if( IS_NPC( ch )
	  && IS_SET( ch->attacks, DFND_DISARM )
	  && number_percent() < ch->top_level / 2 )
	disarm( ch, victim );

      if( IS_NPC( ch )
	  && IS_SET( ch->attacks, ATCK_TRIP )
	  && number_percent() < ch->top_level )
	trip( ch, victim );

      if( check_parry( ch, victim ) )
	return rNONE;
      if( check_dodge( ch, victim ) )
	return rNONE;
    }

    dam_message( ch, victim, dam, dt );
  }

  /*
   * Code to handle equipment getting damaged, and also support  -Thoric
   * bonuses/penalties for having or not having equipment where hit
   */
  if( dam > 10 && dt != TYPE_UNDEFINED )
  {
    /* get a random body eq part */
    dameq = number_range( WEAR_LIGHT, WEAR_EYES );
    damobj = get_eq_char( victim, dameq );
    if( damobj )
    {
      if( dam > get_obj_resistance( damobj ) )
      {
	set_cur_obj( damobj );
	damage_obj( damobj );
      }
      dam -= 5;		/* add a bonus for having something to block the blow */
    }
    else
      dam += 5;		/* add penalty for bare skin! */
  }

  /*
   * Hurt the victim.
   * Inform the victim of his new state.
   */

  victim->hit -= dam;

  if( !IS_NPC( victim ) && IS_IMMORTAL( victim ) && victim->hit < 1 )
    victim->hit = 1;


  if( dam > 0 && dt > TYPE_HIT
      && !IS_AFFECTED( victim, AFF_POISON )
      && is_wielding_poisoned( ch )
      && !IS_SET( victim->immune, RIS_POISON )
      && !saves_poison_death( ch->top_level, victim ) )
  {
    AFFECT_DATA af;

    af.type = gsn_poison;
    af.duration = 20;
    af.location = APPLY_STR;
    af.modifier = -2;
    af.bitvector = AFF_POISON;
    affect_join( victim, &af );
    ch->mental_state = URANGE( 20, ch->mental_state + 2, 100 );
  }

  if( !npcvict
      && IS_IMMORTAL( victim ) && IS_IMMORTAL( ch ) && victim->hit < 1 )
    victim->hit = 1;
  update_pos( victim );

  switch ( victim->position )
  {
    case POS_MORTAL:
      act( AT_DYING,
	  "$n is mortally wounded, and will die soon, if not aided.", victim,
	  NULL, NULL, TO_ROOM );
      send_to_char
	( "&RYou are mortally wounded, and will die soon, if not aided.",
	  victim );
      break;

    case POS_INCAP:
      act( AT_DYING, "$n is incapacitated and will slowly die, if not aided.",
	  victim, NULL, NULL, TO_ROOM );
      send_to_char
	( "&RYou are incapacitated and will slowly die, if not aided.",
	  victim );
      break;

    case POS_STUNNED:
      if( !IS_AFFECTED( victim, AFF_PARALYSIS ) )
      {
	act( AT_ACTION, "$n is stunned, but will probably recover.",
	    victim, NULL, NULL, TO_ROOM );
	send_to_char( "&RYou are stunned, but will probably recover.",
	    victim );
      }
      break;

    case POS_DEAD:
      if( dt >= 0 && dt < top_sn )
      {
	SKILLTYPE *skill = skill_table[dt];

	if( skill->die_char && skill->die_char[0] != '\0' )
	  act( AT_DEAD, skill->die_char, ch, NULL, victim, TO_CHAR );
	if( skill->die_vict && skill->die_vict[0] != '\0' )
	  act( AT_DEAD, skill->die_vict, ch, NULL, victim, TO_VICT );
	if( skill->die_room && skill->die_room[0] != '\0' )
	  act( AT_DEAD, skill->die_room, ch, NULL, victim, TO_NOTVICT );
      }
      if( IS_NPC( victim ) && IS_SET( victim->act, ACT_NOKILL ) )
	act( AT_YELLOW,
	    "$n flees for $s life ... barely escaping certain death!",
	    victim, 0, 0, TO_ROOM );
      else if( IS_NPC( victim ) && IS_SET( victim->act, ACT_DROID ) )
	act( AT_DEAD, "$n EXPLODES into many small pieces!", victim, 0, 0,
	    TO_ROOM );
      else
	act( AT_DEAD, "$n is DEAD!", victim, 0, 0, TO_ROOM );
      send_to_char( "&WYou have been KILLED!\r\n", victim );
      break;

    default:
      if( dam > victim->max_hit / 4 )
      {
	act( AT_HURT, "That really did HURT!", victim, 0, 0, TO_CHAR );
	if( number_bits( 3 ) == 0 )
	  worsen_mental_state( ch, 1 );
      }
      if( victim->hit < victim->max_hit / 4 )

      {
	act( AT_DANGER,
	    "You wish that your wounds would stop BLEEDING so much!",
	    victim, 0, 0, TO_CHAR );
	if( number_bits( 2 ) == 0 )
	  worsen_mental_state( ch, 1 );
      }
      break;
  }

  /*
   * Sleep spells and extremely wounded folks.
   */
  if( !IS_AWAKE( victim )	/* lets make NPC's not slaughter PC's */
      && !IS_AFFECTED( victim, AFF_PARALYSIS ) )
  {
    if( victim->fighting
	&& victim->fighting->who->hunting
	&& victim->fighting->who->hunting->who == victim )
      stop_hunting( victim->fighting->who );

    if( victim->fighting
	&& victim->fighting->who->hating
	&& victim->fighting->who->hating->who == victim )
      stop_hating( victim->fighting->who );

    stop_fighting( victim, TRUE );
  }

  if( victim->hit <= 0 && !IS_NPC( victim ) )
  {
    OBJ_DATA *obj;
    OBJ_DATA *obj_next;
    int cnt = 0;

    REMOVE_BIT( victim->act, PLR_ATTACKER );

    stop_fighting( victim, TRUE );

    if( ( obj = get_eq_char( victim, WEAR_DUAL_WIELD ) ) != NULL )
      unequip_char( victim, obj );
    if( ( obj = get_eq_char( victim, WEAR_WIELD ) ) != NULL )
      unequip_char( victim, obj );
    if( ( obj = get_eq_char( victim, WEAR_HOLD ) ) != NULL )
      unequip_char( victim, obj );
    if( ( obj = get_eq_char( victim, WEAR_MISSILE_WIELD ) ) != NULL )
      unequip_char( victim, obj );
    if( ( obj = get_eq_char( victim, WEAR_LIGHT ) ) != NULL )
      unequip_char( victim, obj );

    for( obj = victim->first_carrying; obj; obj = obj_next )
    {
      obj_next = obj->next_content;

      if( obj->wear_loc == WEAR_NONE )
      {
	if( obj->pIndexData->progtypes & DROP_PROG && obj->count > 1 )
	{
	  ++cnt;
	  separate_obj( obj );
	  obj_from_char( obj );
	  if( !obj_next )
	    obj_next = victim->first_carrying;
	}
	else
	{
	  cnt += obj->count;
	  obj_from_char( obj );
	}
	act( AT_ACTION, "$n drops $p.", victim, obj, NULL, TO_ROOM );
	act( AT_ACTION, "You drop $p.", victim, obj, NULL, TO_CHAR );
	obj = obj_to_room( obj, victim->in_room );
      }
    }

    add_timer( victim, TIMER_RECENTFIGHT, 100, NULL, 0 );

  }

  /*
   * Payoff for killing things.
   */
  if( victim->position == POS_DEAD )
  {
    CHAR_DATA *gch = NULL;

    if( !npcvict )
    {
      sprintf( log_buf, "%s killed by %s at %ld",
	  victim->name,
	  ( IS_NPC( ch ) ? ch->short_descr : ch->name ),
	  victim->in_room->vnum );
      log_string( log_buf );
      to_channel( log_buf, CHANNEL_MONITOR, "Monitor", 2 );

    }
    else if( !IS_NPC( ch ) && IS_NPC( victim ) )
    {
      add_kill( ch, victim );

      /*
       * Add to kill tracker for grouped chars, as well. -Halcyon
       */
      for( gch = ch->in_room->first_person; gch; gch = gch->next_in_room )
	if( is_same_group( gch, ch ) && !IS_NPC( gch ) && gch != ch )
	  add_kill( gch, victim );
    }

    if( !IS_NPC( victim ) || !IS_SET( victim->act, ACT_NOKILL ) )
      loot = legal_loot( ch, victim );
    else
      loot = FALSE;

    set_cur_char( victim );
    raw_kill( ch, victim );
    victim = NULL;

    if( !IS_NPC( ch ) && loot )
    {
      /* Autogold by Scryn 8/12 */
      if( IS_SET( ch->act, PLR_AUTOGOLD ) )
      {
	char buf[MAX_STRING_LENGTH];
	snprintf( buf, MAX_STRING_LENGTH, "%s", "credits corpse" );
	init_gold = ch->gold;
	do_get( ch, buf );
	new_gold = ch->gold;
	gold_diff = ( new_gold - init_gold );

	if( gold_diff > 0 )
	{
	  sprintf( buf1, "%d", gold_diff );
	  do_split( ch, buf1 );
	}
      }

      if( IS_SET( ch->act, PLR_AUTOLOOT ) )
	{
	  char buf[MAX_STRING_LENGTH];
	  snprintf( buf, MAX_STRING_LENGTH, "%s", "all corpse" );
	  do_get( ch, buf );
	}
      else
	{
	  char buf[MAX_STRING_LENGTH];
	  snprintf( buf, MAX_STRING_LENGTH, "%s", "in corpse" );
	  do_look( ch, buf );
	}
    }

    if( IS_SET( sysdata.save_flags, SV_KILL ) )
      save_char_obj( ch );
    return rVICT_DIED;
  }

  if( victim == ch )
    return rNONE;

  /*
   * Take care of link dead people.
   */
  if( !npcvict && !victim->desc && !victim->switched )
  {
    if( number_range( 0, victim->wait ) == 0 )
    {
      int numattempts = 0;
      for( numattempts = 0; numattempts < 5; ++numattempts )
      {
	do_flee( victim, STRLIT_EMPTY );
      }

      do_hail( victim, STRLIT_EMPTY );
      do_quit( victim, STRLIT_EMPTY );
      return rNONE;
    }
  }

  /*
   * Wimp out?
   */
  if( npcvict && dam > 0 )
  {
    if( victim->hit < victim->max_hit / 2
	|| ( IS_AFFECTED( victim, AFF_CHARM ) && victim->master
	  && victim->master->in_room != victim->in_room ) )
    {
      if( IS_SET( victim->act, ACT_WIMPY ) && number_bits( 1 ) == 0 )
      {
	start_fearing( victim, ch );
	stop_hunting( victim );
	do_flee( victim, STRLIT_EMPTY );
      }
    }
  }

  if( !npcvict
      && victim->hit > 0
      && victim->hit <= victim->wimpy && victim->wait == 0 )
    do_flee( victim, STRLIT_EMPTY );
  else if( !npcvict && IS_SET( victim->act, PLR_FLEE ) )
    do_flee( victim, STRLIT_EMPTY );

  return rNONE;
}

bool is_safe( CHAR_DATA * ch, const CHAR_DATA * victim )
{
  if( !victim )
    return FALSE;

  /* Thx Josh! */
  if( who_fighting( ch ) == ch )
    return FALSE;

  if( IS_SET( victim->in_room->room_flags, ROOM_SAFE ) )
  {
    set_char_color( AT_MAGIC, ch );
    send_to_char( "You'll have to do that elsewhere.\r\n", ch );
    return TRUE;
  }

  return FALSE;
}

/*
 * just verify that a corpse looting is legal
 */
bool legal_loot( const CHAR_DATA * ch, const CHAR_DATA * victim )
{
  /* pc's can now loot .. why not .. death is pretty final */
  if( !IS_NPC( ch ) )
    return TRUE;
  /* non-charmed mobs can loot anything */
  if( IS_NPC( ch ) && !ch->master )
    return TRUE;

  return FALSE;
}



/*
 * Set position of a victim.
 */
void update_pos( CHAR_DATA * victim )
{
  if( !victim )
  {
    bug( "update_pos: null victim", 0 );
    return;
  }

  if( victim->hit > 0 )
  {
    if( victim->position <= POS_STUNNED )
      victim->position = POS_STANDING;
    if( IS_AFFECTED( victim, AFF_PARALYSIS ) )
      victim->position = POS_STUNNED;
    return;
  }

  if( IS_NPC( victim ) || victim->hit <= -100 )
  {
    if( victim->mount )
    {
      act( AT_ACTION, "$n falls from $N.",
	  victim, NULL, victim->mount, TO_ROOM );
      REMOVE_BIT( victim->mount->act, ACT_MOUNTED );
      victim->mount = NULL;
    }
    victim->position = POS_DEAD;
    return;
  }

  if( victim->hit <= -80 )
    victim->position = POS_MORTAL;
  else if( victim->hit <= -50 )
    victim->position = POS_INCAP;
  else
    victim->position = POS_STUNNED;

  if( victim->position > POS_STUNNED && IS_AFFECTED( victim, AFF_PARALYSIS ) )
    victim->position = POS_STUNNED;

  if( victim->mount )
  {
    act( AT_ACTION, "$n falls unconscious from $N.",
	victim, NULL, victim->mount, TO_ROOM );
    REMOVE_BIT( victim->mount->act, ACT_MOUNTED );
    victim->mount = NULL;
  }
  return;
}


/*
 * Start fights.
 */
void set_fighting( CHAR_DATA * ch, CHAR_DATA * victim )
{
  FIGHT_DATA *fight;

  if( ch->fighting )
  {
    char buf[MAX_STRING_LENGTH];

    sprintf( buf, "Set_fighting: %s -> %s (already fighting %s)",
	ch->name, victim->name, ch->fighting->who->name );
    bug( buf, 0 );
    return;
  }

  if( IS_AFFECTED( ch, AFF_SLEEP ) )
    affect_strip( ch, gsn_sleep );

  /* Limit attackers -Thoric */
  if( victim->num_fighting > max_fight( victim ) )
  {
    send_to_char
      ( "There are too many people fighting for you to join in.\r\n", ch );
    return;
  }

  CREATE( fight, FIGHT_DATA, 1 );
  fight->who = victim;
  fight->align = align_compute( ch, victim );
  if( !IS_NPC( ch ) && IS_NPC( victim ) )
    fight->timeskilled = times_killed( ch, victim );
  ch->num_fighting = 1;
  ch->fighting = fight;
  ch->position = POS_FIGHTING;
  victim->num_fighting++;
  if( victim->switched && IS_AFFECTED( victim->switched, AFF_POSSESS ) )
  {
    send_to_char( "You are disturbed!\r\n", victim->switched );
    do_return( victim->switched, STRLIT_EMPTY );
  }
  return;
}


CHAR_DATA *who_fighting( const CHAR_DATA * ch )
{
  if( !ch )
  {
    bug( "who_fighting: null ch", 0 );
    return NULL;
  }

  if( !ch->fighting )
    return NULL;

  return ch->fighting->who;
}

void free_fight( CHAR_DATA * ch )
{
  if( !ch )
  {
    bug( "Free_fight: null ch!", 0 );
    return;
  }
  if( ch->fighting )
  {
    if( !char_died( ch->fighting->who ) )
      --ch->fighting->who->num_fighting;
    DISPOSE( ch->fighting );
  }
  ch->fighting = NULL;
  if( ch->mount )
    ch->position = POS_MOUNTED;
  else
    ch->position = POS_STANDING;
  /* Berserk wears off after combat. -- Altrag */
  if( IS_AFFECTED( ch, AFF_BERSERK ) )
  {
    affect_strip( ch, gsn_berserk );
    set_char_color( AT_WEAROFF, ch );
    send_to_char( skill_table[gsn_berserk]->msg_off, ch );
    send_to_char( "\r\n", ch );
  }
  return;
}


/*
 * Stop fights.
 */
void stop_fighting( CHAR_DATA * ch, bool fBoth )
{
  CHAR_DATA *fch;

  free_fight( ch );
  update_pos( ch );

  if( !fBoth )			/* major short cut here by Thoric */
    return;

  for( fch = first_char; fch; fch = fch->next )
  {
    if( who_fighting( fch ) == ch )
    {
      free_fight( fch );
      update_pos( fch );
    }
  }
  return;
}

void raw_kill( CHAR_DATA * ch, CHAR_DATA * victim )
{
  CHAR_DATA *victmp = NULL;
  char filename[256];
  char buf[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH];
  char arg[MAX_STRING_LENGTH];
  SHIP_DATA *ship = NULL;
  char diemsg[MAX_STRING_LENGTH];

  if( !victim )
  {
    bug( "raw_kill: null victim!", 0 );
    return;
  }

  strcpy( arg, victim->name );

  stop_fighting( victim, TRUE );


  /* Take care of polymorphed chars */
  if( IS_NPC( victim ) && IS_SET( victim->act, ACT_POLYMORPHED ) )
  {
    char_from_room( victim->desc->original );
    char_to_room( victim->desc->original, victim->in_room );
    victmp = victim->desc->original;
    do_revert( victim, STRLIT_EMPTY );
    raw_kill( ch, victmp );
    return;
  }

  if( victim->in_room && IS_NPC( victim ) && victim->in_room->area
      && victim->in_room->area->planet )
  {

    if( victim->guard_data )
    {
      GUARD_DATA *guard = victim->guard_data;

      if( guard->planet )
	UNLINK( guard, guard->planet->first_guard,
	    guard->planet->last_guard, next_on_planet,
	    prev_on_planet );
      UNLINK( guard, first_guard, last_guard, next, prev );

      victim->guard_data = NULL;
      DISPOSE( guard );
    }
    else if( IS_SET( victim->act, ACT_CITIZEN ) )
    {
      victim->in_room->area->planet->population--;
      victim->in_room->area->planet->population =
	UMAX( victim->in_room->area->planet->population, 0 );
      victim->in_room->area->planet->pop_support -=
	( float ) ( 1 +
		    1 / ( victim->in_room->area->planet->population +
		      1 ) );
      if( victim->in_room->area->planet->pop_support < -100 )
	victim->in_room->area->planet->pop_support = -100;
    }
    else
    {
      victim->in_room->area->planet->wildlife--;
      victim->in_room->area->planet->wildlife =
	UMAX( victim->in_room->area->planet->wildlife, 0 );
    }
  }

  if( !IS_NPC( victim ) || !IS_SET( victim->act, ACT_NOKILL ) )
    mprog_death_trigger( ch, victim );
  if( char_died( victim ) )
    return;

  if( !IS_NPC( victim ) || !IS_SET( victim->act, ACT_NOKILL ) )
    rprog_death_trigger( ch, victim );
  if( char_died( victim ) )
    return;

  if( !IS_NPC( victim )
      || ( !IS_SET( victim->act, ACT_NOKILL )
	&& !IS_SET( victim->act, ACT_NOCORPSE ) ) )
    make_corpse( victim );
  else
  {
    character_extract_carried_objects( victim );
  }

  /*    make_blood( victim ); */

  if( IS_NPC( victim ) )
  {
    victim->pIndexData->killed++;
    extract_char( victim, TRUE );
    victim = NULL;
    return;
  }

  set_char_color( AT_DIEMSG, victim );
  snprintf( diemsg, MAX_STRING_LENGTH, "%s", "_DIEMSG_" );
  do_help( victim, diemsg );

  for( ship = first_ship; ship; ship = ship->next )
  {
    if( !str_cmp( ship->owner, victim->name ) )
    {
      STRFREE( ship->owner );
      ship->owner = STRALLOC( "" );
      STRFREE( ship->pilot );
      ship->pilot = STRALLOC( "" );
      STRFREE( ship->copilot );
      ship->copilot = STRALLOC( "" );

      save_ship( ship );
      write_ship_list();
    }

  }


  if( victim->plr_home )
  {
    ROOM_INDEX_DATA *room = victim->plr_home;

    STRFREE( room->name );
    room->name = STRALLOC( "An Empty Apartment" );

    REMOVE_BIT( room->room_flags, ROOM_PLR_HOME );
    SET_BIT( room->room_flags, ROOM_EMPTY_HOME );

    if( room->area )
    {
      sprintf( filename, "%s%s", AREA_DIR, room->area->filename );
      fold_area( room->area, filename, FALSE );
    }
  }

  if( victim->pcdata && victim->pcdata->clan )
  {
    if( clan_char_is_leader( victim->pcdata->clan, victim ) )
    {
      clan_remove_leader( victim->pcdata->clan, victim->name );
    }

    if( !NOT_AUTHED( victim ) )
      victim->pcdata->clan->members--;
  }

  if( !victim )
  {
    DESCRIPTOR_DATA *d;

    /* Make sure they aren't halfway logged in. */
    for( d = first_descriptor; d; d = d->next )
      if( ( victim = d->character ) && !IS_NPC( victim ) )
	break;
    if( d )
      close_socket( d, TRUE );
  }
  else
  {
    int x, y;

    quitting_char = victim;
    save_char_obj( victim );
    saving_char = NULL;
    extract_char( victim, TRUE );
    for( x = 0; x < MAX_WEAR; x++ )
      for( y = 0; y < MAX_LAYERS; y++ )
	save_equipment[x][y] = NULL;
  }

  sprintf( buf, "%s%c/%s", PLAYER_DIR, tolower( ( int ) arg[0] ),
      capitalize( arg ) );
  sprintf( buf2, "%s%c/%s", BACKUP_DIR, tolower( ( int ) arg[0] ),
      capitalize( arg ) );

  rename( buf, buf2 );

  sprintf( buf, "%s%c/%s.clone", PLAYER_DIR, tolower( ( int ) arg[0] ),
      capitalize( arg ) );
  sprintf( buf2, "%s%c/%s", PLAYER_DIR, tolower( ( int ) arg[0] ),
      capitalize( arg ) );

  rename( buf, buf2 );

  return;
}

int align_compute( const CHAR_DATA * gch, const CHAR_DATA * victim )
{
  return URANGE( -1000,
      ( int ) ( gch->alignment - victim->alignment / 5 ), 1000 );
}

void dam_message( CHAR_DATA * ch, CHAR_DATA * victim, int dam, int dt )
{
  char buf1[256], buf2[256], buf3[256];
  const char *vs = NULL;
  const char *vp = NULL;
  const char *attack = NULL;
  char punct = 0;
  int dampc = 0;
  SKILLTYPE *skill = NULL;
  bool gcflag = FALSE;
  bool gvflag = FALSE;

  if( !dam )
    dampc = 0;
  else
    dampc = ( ( dam * 1000 ) / victim->max_hit ) +
      ( 50 - ( ( victim->hit * 50 ) / victim->max_hit ) );

  /*                10 * percent                                       */
  if( dam == 0 )
  {
    vs = "miss";
    vp = "misses";
  }
  else if( dampc <= 2 )
  {
    vs = "barely scratch";
    vp = "barely scratches";
  }
  else if( dampc <= 4 )
  {
    vs = "scratch";
    vp = "scratches";
  }
  else if( dampc <= 6 )
  {
    vs = "nick";
    vp = "nicks";
  }
  else if( dampc <= 8 )
  {
    vs = "graze";
    vp = "grazes";
  }
  else if( dampc <= 10 )
  {
    vs = "bruise";
    vp = "bruises";
  }
  else if( dampc <= 15 )
  {
    vs = "hit";
    vp = "hits";
  }
  else if( dampc <= 20 )
  {
    vs = "injure";
    vp = "injures";
  }
  else if( dampc <= 25 )
  {
    vs = "thrash";
    vp = "thrashes";
  }
  else if( dampc <= 30 )
  {
    vs = "wound";
    vp = "wounds";
  }
  else if( dampc <= 35 )
  {
    vs = "maul";
    vp = "mauls";
  }
  else if( dampc <= 50 )
  {
    vs = "decimate";
    vp = "decimates";
  }
  else if( dampc <= 60 )
  {
    vs = "devastate";
    vp = "devastates";
  }
  else if( dampc <= 70 )
  {
    vs = "maim";
    vp = "maims";
  }
  else if( dampc <= 80 )
  {
    vs = "MUTILATE";
    vp = "MUTILATES";
  }
  else if( dampc <= 90 )
  {
    vs = "DISEMBOWEL";
    vp = "DISEMBOWELS";
  }
  else if( dampc <= 100 )
  {
    vs = "MASSACRE";
    vp = "MASSACRES";
  }
  else if( dampc <= 125 )
  {
    vs = "PULVERIZE";
    vp = "PULVERIZES";
  }
  else if( dampc <= 150 )
  {
    vs = "EVISCERATE";
    vp = "EVISCERATES";
  }
  else if( dampc <= 200 )
  {
    vs = "* OBLITERATE *";
    vp = "* OBLITERATES *";
  }
  else
  {
    vs = "*** ANNIHILATE ***";
    vp = "*** ANNIHILATES ***";
  }

  punct = ( dampc <= 30 ) ? '.' : '!';

  if( dam == 0 && ( !IS_NPC( ch ) &&
	( IS_SET( ch->pcdata->flags, PCFLAG_GAG ) ) ) )
    gcflag = TRUE;

  if( dam == 0 && ( !IS_NPC( victim ) &&
	( IS_SET( victim->pcdata->flags, PCFLAG_GAG ) ) ) )
    gvflag = TRUE;

  if( dt >= 0 && dt < top_sn )
    skill = skill_table[dt];

  if( dt == ( TYPE_HIT + WEAPON_BLASTER ) )
  {
    char sound[MAX_STRING_LENGTH];
    int vol = number_range( 20, 80 );

    sprintf( sound, "!!SOUND(blaster V=%d)", vol );
    sound_to_room( ch->in_room, sound );
  }

  if( dt == TYPE_HIT || dam == 0 )
  {
    sprintf( buf1, "$n %s $N%c", vp, punct );
    sprintf( buf2, "You %s $N%c", vs, punct );
    sprintf( buf3, "$n %s you%c", vp, punct );
  }
  else if( dt > TYPE_HIT && is_wielding_poisoned( ch ) )
  {
    if( dt < ( int ) ( TYPE_HIT + attack_table_size() ) )
    {
      attack = attack_table[dt - TYPE_HIT];
    }
    else
    {
      bug( "Dam_message: bad dt %d.", dt );
      dt = TYPE_HIT;
      attack = attack_table[0];
    }

    sprintf( buf1, "$n's poisoned %s %s $N%c", attack, vp, punct );
    sprintf( buf2, "Your poisoned %s %s $N%c", attack, vp, punct );
    sprintf( buf3, "$n's poisoned %s %s you%c", attack, vp, punct );
  }
  else
  {
    if( skill )
    {
      attack = skill->noun_damage;
      if( dam == 0 )
      {
	bool found = FALSE;

	if( skill->miss_char && skill->miss_char[0] != '\0' )
	{
	  act( AT_HIT, skill->miss_char, ch, NULL, victim, TO_CHAR );
	  found = TRUE;
	}
	if( skill->miss_vict && skill->miss_vict[0] != '\0' )
	{
	  act( AT_HITME, skill->miss_vict, ch, NULL, victim,
	      TO_VICT );
	  found = TRUE;
	}
	if( skill->miss_room && skill->miss_room[0] != '\0' )
	{
	  act( AT_ACTION, skill->miss_room, ch, NULL, victim,
	      TO_NOTVICT );
	  found = TRUE;
	}
	if( found )	/* miss message already sent */
	  return;
      }
      else
      {
	if( skill->hit_char && skill->hit_char[0] != '\0' )
	  act( AT_HIT, skill->hit_char, ch, NULL, victim, TO_CHAR );
	if( skill->hit_vict && skill->hit_vict[0] != '\0' )
	  act( AT_HITME, skill->hit_vict, ch, NULL, victim, TO_VICT );
	if( skill->hit_room && skill->hit_room[0] != '\0' )
	  act( AT_ACTION, skill->hit_room, ch, NULL, victim,
	      TO_NOTVICT );
      }
    }
    else if( dt >= TYPE_HIT
	     && dt < ( int ) ( TYPE_HIT + attack_table_size() ) )
      attack = attack_table[dt - TYPE_HIT];
    else
    {
      bug( "Dam_message: bad dt %d.", dt );
      dt = TYPE_HIT;
      attack = attack_table[0];
    }

    sprintf( buf1, "$n's %s %s $N%c", attack, vp, punct );
    sprintf( buf2, "Your %s %s $N%c", attack, vp, punct );
    sprintf( buf3, "$n's %s %s you%c", attack, vp, punct );
  }

  act( AT_ACTION, buf1, ch, NULL, victim, TO_NOTVICT );
  if( !gcflag )
    act( AT_HIT, buf2, ch, NULL, victim, TO_CHAR );
  if( !gvflag )
    act( AT_HITME, buf3, ch, NULL, victim, TO_VICT );

  return;
}


void do_kill( CHAR_DATA * ch, char *argument )
{
  char arg[MAX_INPUT_LENGTH];
  CHAR_DATA *victim;

  one_argument( argument, arg );

  if( arg[0] == '\0' )
  {
    send_to_char( "Kill whom?\r\n", ch );
    return;
  }

  if( ( victim = get_char_room( ch, arg ) ) == NULL )
  {
    send_to_char( "They aren't here.\r\n", ch );
    return;
  }

  if( !IS_NPC( victim ) )
  {
    send_to_char( "You must MURDER a player.\r\n", ch );
    return;
  }

  if( victim == ch )
  {
    send_to_char( "You hit yourself.  Ouch!\r\n", ch );
    multi_hit( ch, ch, TYPE_UNDEFINED );
    return;
  }

  if( is_safe( ch, victim ) )
    return;

  if( IS_AFFECTED( ch, AFF_CHARM ) && ch->master == victim )
  {
    act( AT_PLAIN, "$N is your beloved master.", ch, NULL, victim,
	TO_CHAR );
    return;
  }

  if( ch->position == POS_FIGHTING )
  {
    send_to_char( "You do the best you can!\r\n", ch );
    return;
  }

  WAIT_STATE( ch, 1 * PULSE_VIOLENCE );
  multi_hit( ch, victim, TYPE_UNDEFINED );
  return;
}

void do_murder( CHAR_DATA * ch, char *argument )
{
  char arg[MAX_INPUT_LENGTH];
  CHAR_DATA *victim;

  one_argument( argument, arg );

  if( arg[0] == '\0' )
  {
    send_to_char( "Murder whom?\r\n", ch );
    return;
  }

  if( ( victim = get_char_room( ch, arg ) ) == NULL )
  {
    send_to_char( "They aren't here.\r\n", ch );
    return;
  }

  if( victim == ch )
  {
    send_to_char( "Suicide is a mortal sin.\r\n", ch );
    return;
  }

  if( is_safe( ch, victim ) )
    return;

  if( IS_AFFECTED( ch, AFF_CHARM ) )
  {
    if( ch->master == victim )
    {
      act( AT_PLAIN, "$N is your beloved master.", ch, NULL, victim,
	  TO_CHAR );
      return;
    }
  }

  if( ch->position == POS_FIGHTING )
  {
    send_to_char( "You do the best you can!\r\n", ch );
    return;
  }

  if( !IS_NPC( victim ) && IS_SET( ch->act, PLR_NICE ) )
  {
    send_to_char( "You feel too nice to do that!\r\n", ch );
    return;
  }

  ch->alignment = URANGE( -1000, ch->alignment - 10, 1000 );

  WAIT_STATE( ch, 1 * PULSE_VIOLENCE );
  multi_hit( ch, victim, TYPE_UNDEFINED );
}

void do_flee( CHAR_DATA * ch, char *argument )
{
  ROOM_INDEX_DATA *was_in;
  ROOM_INDEX_DATA *now_in;
  char buf[MAX_STRING_LENGTH];
  int attempt;
  short door;
  EXIT_DATA *pexit;

  if( !who_fighting( ch ) )
  {
    if( ch->position == POS_FIGHTING )
    {
      if( ch->mount )
	ch->position = POS_MOUNTED;
      else
	ch->position = POS_STANDING;
    }
    send_to_char( "You aren't fighting anyone.\r\n", ch );
    return;
  }

  if( ch->move <= 0 )
  {
    send_to_char( "You're too exhausted to flee from combat!\r\n", ch );
    return;
  }

  /* No fleeing while stunned. - Narn */
  if( ch->position < POS_FIGHTING )
    return;

  was_in = ch->in_room;
  for( attempt = 0; attempt < 8; attempt++ )
  {

    door = number_door();
    if( ( pexit = get_exit( was_in, door ) ) == NULL
	|| !pexit->to_room
	|| ( IS_SET( pexit->exit_info, EX_CLOSED )
	  && !IS_AFFECTED( ch, AFF_PASS_DOOR ) )
	|| ( IS_NPC( ch )
	  && IS_SET( pexit->to_room->room_flags, ROOM_NO_MOB ) ) )
      continue;

    affect_strip( ch, gsn_sneak );
    REMOVE_BIT( ch->affected_by, AFF_SNEAK );
    if( ch->mount && ch->mount->fighting )
      stop_fighting( ch->mount, TRUE );
    move_char( ch, pexit, 0 );
    if( ( now_in = ch->in_room ) == was_in )
      continue;

    ch->in_room = was_in;
    act( AT_FLEE, "$n runs for cover!", ch, NULL, NULL, TO_ROOM );
    ch->in_room = now_in;
    act( AT_FLEE, "$n glances around for signs of pursuit.", ch, NULL, NULL,
	TO_ROOM );
    sprintf( buf, "You run for cover!" );
    send_to_char( buf, ch );

    stop_fighting( ch, TRUE );
    return;
  }

  sprintf( buf, "You attempt to run for cover!" );
  send_to_char( buf, ch );
  return;
}

bool get_cover( CHAR_DATA * ch )
{
  ROOM_INDEX_DATA *was_in;
  ROOM_INDEX_DATA *now_in;
  int attempt;
  short door;
  EXIT_DATA *pexit;

  if( !who_fighting( ch ) )
    return FALSE;

  if( ch->position < POS_FIGHTING )
    return FALSE;

  was_in = ch->in_room;
  for( attempt = 0; attempt < 10; attempt++ )
  {

    door = number_door();
    if( ( pexit = get_exit( was_in, door ) ) == NULL
	|| !pexit->to_room
	|| ( IS_SET( pexit->exit_info, EX_CLOSED )
	  && !IS_AFFECTED( ch, AFF_PASS_DOOR ) )
	|| ( IS_NPC( ch )
	  && IS_SET( pexit->to_room->room_flags, ROOM_NO_MOB ) ) )
      continue;

    affect_strip( ch, gsn_sneak );
    REMOVE_BIT( ch->affected_by, AFF_SNEAK );
    if( ch->mount && ch->mount->fighting )
      stop_fighting( ch->mount, TRUE );
    move_char( ch, pexit, 0 );
    if( ( now_in = ch->in_room ) == was_in )
      continue;

    ch->in_room = was_in;
    act( AT_FLEE, "$n sprints for cover!", ch, NULL, NULL, TO_ROOM );
    ch->in_room = now_in;
    act( AT_FLEE, "$n spins around and takes aim.", ch, NULL, NULL,
	TO_ROOM );

    stop_fighting( ch, TRUE );

    return TRUE;
  }

  return FALSE;
}

void do_slay( CHAR_DATA * ch, char *argument )
{
  CHAR_DATA *victim;
  char arg[MAX_INPUT_LENGTH];
  char arg2[MAX_INPUT_LENGTH];

  argument = one_argument( argument, arg );
  one_argument( argument, arg2 );
  if( arg[0] == '\0' )
  {
    send_to_char( "Slay whom?\r\n", ch );
    return;
  }

  if( ( victim = get_char_room( ch, arg ) ) == NULL )
  {
    send_to_char( "They aren't here.\r\n", ch );
    return;
  }

  if( ch == victim )
  {
    send_to_char( "Suicide is a mortal sin.\r\n", ch );
    return;
  }

  if( !IS_NPC( victim )
      && ( get_trust( victim ) == 103 || get_trust( ch ) < 103 ) )
  {
    send_to_char( "You failed.\r\n", ch );
    return;
  }

  if( !str_cmp( arg2, "immolate" ) )
  {
    act( AT_FIRE, "Your fireball turns $N into a blazing inferno.", ch,
	NULL, victim, TO_CHAR );
    act( AT_FIRE, "$n releases a searing fireball in your direction.", ch,
	NULL, victim, TO_VICT );
    act( AT_FIRE, "$n points at $N, who bursts into a flaming inferno.", ch,
	NULL, victim, TO_NOTVICT );
  }

  else if( !str_cmp( arg2, "shatter" ) )
  {
    act( AT_LBLUE,
	"You freeze $N with a glance and shatter the frozen corpse into tiny shards.",
	ch, NULL, victim, TO_CHAR );
    act( AT_LBLUE,
	"$n freezes you with a glance and shatters your frozen body into tiny shards.",
	ch, NULL, victim, TO_VICT );
    act( AT_LBLUE,
	"$n freezes $N with a glance and shatters the frozen body into tiny shards.",
	ch, NULL, victim, TO_NOTVICT );
  }

  else if( !str_cmp( arg2, "demon" ) )
  {
    act( AT_IMMORT,
	"You gesture, and a slavering demon appears.  With a horrible grin, the",
	ch, NULL, victim, TO_CHAR );
    act( AT_IMMORT,
	"foul creature turns on $N, who screams in panic before being eaten alive.",
	ch, NULL, victim, TO_CHAR );
    act( AT_IMMORT,
	"$n gestures, and a slavering demon appears.  The foul creature turns on",
	ch, NULL, victim, TO_VICT );
    act( AT_IMMORT,
	"you with a horrible grin.   You scream in panic before being eaten alive.",
	ch, NULL, victim, TO_VICT );
    act( AT_IMMORT,
	"$n gestures, and a slavering demon appears.  With a horrible grin, the",
	ch, NULL, victim, TO_NOTVICT );
    act( AT_IMMORT,
	"foul creature turns on $N, who screams in panic before being eaten alive.",
	ch, NULL, victim, TO_NOTVICT );
  }

  else if( !str_cmp( arg2, "pounce" ) )
  {
    act( AT_BLOOD,
	"Leaping upon $N with bared fangs, you tear open $S throat and toss the corpse to the ground...",
	ch, NULL, victim, TO_CHAR );
    act( AT_BLOOD,
	"In a heartbeat, $n rips $s fangs through your throat!  Your blood sprays and pours to the ground as your life ends...",
	ch, NULL, victim, TO_VICT );
    act( AT_BLOOD,
	"Leaping suddenly, $n sinks $s fangs into $N's throat.  As blood sprays and gushes to the ground, $n tosses $N's dying body away.",
	ch, NULL, victim, TO_NOTVICT );
  }

  else if( !str_cmp( arg2, "slit" ) )
  {
    act( AT_BLOOD, "You calmly slit $N's throat.", ch, NULL, victim,
	TO_CHAR );
    act( AT_BLOOD,
	"$n reaches out with a clawed finger and calmly slits your throat.",
	ch, NULL, victim, TO_VICT );
    act( AT_BLOOD, "$n calmly slits $N's throat.", ch, NULL, victim,
	TO_NOTVICT );
  }

  else
  {
    act( AT_IMMORT, "You slay $N in cold blood!", ch, NULL, victim,
	TO_CHAR );
    act( AT_IMMORT, "$n slays you in cold blood!", ch, NULL, victim,
	TO_VICT );
    act( AT_IMMORT, "$n slays $N in cold blood!", ch, NULL, victim,
	TO_NOTVICT );
  }

  set_cur_char( victim );
  raw_kill( ch, victim );
  return;
}
