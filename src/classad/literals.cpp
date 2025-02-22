/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "classad/common.h"
#include "classad/exprTree.h"
#include "classad/util.h"

using namespace std;

namespace classad {

static inline void nextDigitChar(const string &Str, int &index);
static inline void prevNonSpaceChar(const string &Str, int &index);
static int revInt(const string &revNumStr);
static double revDouble(const string &revNumStr);
static bool extractTimeZone(string &timeStr, int &tzhr, int &tzmin);


void Literal::setError(int err, const char *msg /*=NULL*/)
{
	CondorErrno = err;
	CondorErrMsg = msg ? msg : "";
}


Literal* Literal::
MakeReal(const string &number_string) 
{
	char   *end;
	double real = strtod(number_string.c_str(), &end);
	if (end == number_string.c_str() && real == 0.0) {
		return MakeUndefined();
	}
	return MakeReal(real);
}

Literal* Literal::
MakeAbsTime( abstime_t *tim )
{
    Value val;
    abstime_t abst;
    if (tim == NULL) { // => current time/offset
        time_t now;
        time( &now );
        abst.secs = now;
        abst.offset = timezone_offset( now, false );
    }
    else { //make a literal out of the passed value
        abst = *tim;
    }
    val.SetAbsoluteTimeValue( abst);
    return( MakeLiteral( val ) );
}

/* Creates an absolute time literal, from the string timestr, 
 *parsing it as the regular expression:
 D* dddd [D* dd [D* dd [D* dd [D* dd [D* dd D*]]]]] [-dd:dd | +dd:dd | z | Z]
 D => non-digit, d=> digit
 Ex - 2003-01-25T09:00:00-06:00
*/
Literal* Literal::
MakeAbsTime(string timeStr )
{    
	abstime_t abst;
	Value val;
	bool offset = false; // to check if the argument conatins a timezone offset parameter
	
	struct tm abstm;
    memset(&abstm, 0, sizeof(abstm));
	int tzhr = 0; // corresponds to 1st "dd" in -|+dd:dd
	int tzmin = 0; // corresponds to 2nd "dd" in -|+dd:dd
	
	int len = (int)timeStr.length();
	int i=len-1; 
	prevNonSpaceChar(timeStr,i);
	if ((i > 0) && ((timeStr[i] == 'z') || (timeStr[i] == 'Z'))) { // z|Z corresponds to a timezone offset of 0
		offset = true;
		timeStr.erase(i,1); // remove the offset section from the string
		tzhr = 0;
		tzmin = 0;
    } else if ((len > 5) && (timeStr[len-5] == '+' || timeStr[len-5] == '-')) {
        offset = extractTimeZone(timeStr, tzhr, tzmin);
    } else if ((len > 6) && ((timeStr[len-6] == '+' || timeStr[len-6] == '-') && timeStr[len-3] == ':')) {
        timeStr.erase(len-3, 1);
        offset = extractTimeZone(timeStr, tzhr, tzmin);
    }

	i=0;
	len = (int)timeStr.length();
	
	nextDigitChar(timeStr,i);
	if(i > len-4) { // string has to contain dddd (year)
		val.SetErrorValue( );
		return(MakeLiteral( val ));
	}    
	
	abstm.tm_year = atoi(timeStr.substr(i,4).c_str()) - 1900;
	i += 4;
	nextDigitChar(timeStr,i);
	
	if(i<=len-2) {
		abstm.tm_mon = atoi(timeStr.substr(i,2).c_str()) - 1;
		i += 2;
	}
	nextDigitChar(timeStr,i);
	
	if(i<=len-2) {
		abstm.tm_mday = atoi(timeStr.substr(i,2).c_str());	
		i += 2;
	}
	nextDigitChar(timeStr,i);
	
	if(i<=len-2) {
		abstm.tm_hour += atoi(timeStr.substr(i,2).c_str()); 
		i += 2;
	}  
	nextDigitChar(timeStr,i);
	
	if(i<=len-2) {
		abstm.tm_min += atoi(timeStr.substr(i,2).c_str());
		i += 2;
	}  
	nextDigitChar(timeStr,i);
	
	if(i<=len-2) {
		abstm.tm_sec = atoi(timeStr.substr(i,2).c_str());	
		i += 2;
	}
	nextDigitChar(timeStr,i);
	
	if((i<=len-1) && (isdigit(timeStr[i]))) {  // there should be no more digit characters once the required
		val.SetErrorValue( );                             // parameteres are parsed
		return(MakeLiteral( val ));
	}      
	
	abst.secs = mktime(&abstm);
	
	if(abst.secs == -1)  { // the time should be one, which can be supported by the time_t type
		val.SetErrorValue( );
		return(MakeLiteral( val ));
	}      

	if(offset) {
		abst.offset = (tzhr*3600) + (tzmin*60);
	}
	else { // if offset is not specified, the offset of the current locality is taken
		abst.offset = findOffset(abst.secs);
		//abst.secs -= abst.offset;
	}

	// mktime() creates the time assuming we specified something in
	// local time.  We want the time as if we were in Greenwich (we'll
	// call gmTime later to extract it, not localtime()), so we adjust
	// by our timezone.
	abst.secs += timezone_offset( abst.secs, true );

	// If the time string we converted had a timezone offset (either
	// explicit or implicit), we need to adjust the resulting time_t
	// so that our final value is UTC.
	abst.secs -= abst.offset;
	
	if(abst.offset == -1) { // corresponds to illegal offset
		val.SetErrorValue( );
		return(MakeLiteral( val ) );
	}
	else {
		val.SetAbsoluteTimeValue(abst);
	}
	
	return( MakeLiteral( val ) );
}

Literal* Literal::
MakeRelTime( time_t t1, time_t t2 )
{
	Value	val;

	if( t1<0 ) time( &t1 );
	if( t2<0 ) time( &t2 );
	val.SetRelativeTimeValue( t1 - t2 );
	return( MakeLiteral( val ) );
}


Literal* Literal::
MakeRelTime( time_t secs )
{
	Value		val;
	struct	tm 	lt;

	if( secs<0 ) {
		time(&secs );
		getLocalTime( &secs, &lt );
		val.SetRelativeTimeValue((time_t) (lt.tm_hour*3600 + lt.tm_min*60 + lt.tm_sec));
	} else {
		val.SetRelativeTimeValue((time_t) secs);
	}
	return( MakeLiteral( val ) );
}

/* Creates a relative time literal, from the string timestr, 
 *parsing it as [[[days+]hh:]mm:]ss
 * Ex - 1+00:02:00
 */
Literal* Literal::
MakeRelTime(const string &timeStr)
{
	Value val;  
	double rsecs;
	
	int len = (int)timeStr.length();
	double secs = 0;
	int mins = 0;
	int hrs = 0;
	int days = 0;
	bool negative = false;
    
	int i=len-1; 
	prevNonSpaceChar(timeStr,i);
	// checking for 'sec' parameter & collecting it if present (ss.sss)
	if((i>=0) &&((timeStr[i] == 's') || (timeStr[i] == 'S') || (isdigit(timeStr[i])))) {
		if((timeStr[i] == 's') || (timeStr[i] == 'S')) {
			i--;
		}
		prevNonSpaceChar(timeStr,i);
		string revSecStr;
		while((i>=0) &&(isdigit(timeStr[i]))) {
			revSecStr += timeStr[i--];
		}
		if((i>=0) &&(timeStr[i] == '.')) {
			revSecStr += timeStr[i--];
			while((i>=0) && (isdigit(timeStr[i]))) {
				revSecStr += timeStr[i--];
			}
		}
		secs = revDouble(revSecStr);
	}
	
	prevNonSpaceChar(timeStr,i);
	// checking for 'min' parameter
	if((i>=0) &&((timeStr[i] == 'm') || (timeStr[i] == 'M') || (timeStr[i] == ':'))) {
		i--;
		string revMinStr;
		prevNonSpaceChar(timeStr,i);
		while((i>=0) &&(isdigit(timeStr[i]))) {
			revMinStr += timeStr[i--];
		}
		mins = revInt(revMinStr);
	}
	
	prevNonSpaceChar(timeStr,i);
	// checking for 'hrs' parameter
	if((i>=0) &&((timeStr[i] == 'h') || (timeStr[i] == 'H') || (timeStr[i] == ':'))) {
		i--;
		string revHrStr;
		prevNonSpaceChar(timeStr,i);
		while((i>=0) &&(isdigit(timeStr[i]))) {
			revHrStr += timeStr[i--];
		}
		hrs = revInt(revHrStr);
	}   
	
	prevNonSpaceChar(timeStr,i);
	// checking for 'days' parameter
	if((i>=0) &&((timeStr[i] == 'd') || (timeStr[i] == 'D') || (timeStr[i] == '+'))) {
		i--;
		string revDayStr;
		prevNonSpaceChar(timeStr,i);
		while((i>=0) &&(isdigit(timeStr[i]))) {
			revDayStr += timeStr[i--];
		}
		days = revInt(revDayStr);
	}     
	
	prevNonSpaceChar(timeStr,i);
	// checking for '-' operator
	if((i>=0) &&(timeStr[i] == '-')) {
		negative = true;
		i--;
	}
	
	prevNonSpaceChar(timeStr,i);
    
	if((i>=0) && (!(isspace(timeStr[i])))) { // should not conatin any non-space char beyond -,d,h,m,s
		val.SetErrorValue( );
		return(MakeLiteral( val ));
	}   
	
	rsecs = ( negative ? -1 : +1 ) * ( days*86400 + hrs*3600 + mins*60 + secs );
	val.SetRelativeTimeValue(rsecs);
	
	return( MakeLiteral( val ) );
}

/* Function which iterates through the string Str from the location 'index', 
 *returning the index of the next digit-char 
 */
static inline void nextDigitChar(const string &Str, int &index) 
{
	int len = (int)Str.length();
    while((index<len) &&(!isdigit(Str[index]))) {
		index++;
    }
}


/* Function which iterates through the string Str backwards from the location 'index'
 *returning the index of the first occuring non-space character
 */
static inline void prevNonSpaceChar(const string &Str, int &index) 
{
    while((index>=0) &&(isspace(Str[index]))) {
		index--;
    }
}


/* Function which takes a number in string format, and reverses the
 * order of the digits & returns the corresponding number as an
 * integer.
 */
static int revInt(const string &revNumStr) 
{
	string numStr = "";
    int number;

	int len = (int)revNumStr.length();
	for(int i=len-1; i>=0 ; i--) {
		numStr += revNumStr[i];
	}

    number = atoi(numStr.c_str());
	return number;
}

/* Function which takes a number in string format, and reverses the
 * order of the digits & returns the corresponding number as a double.
 */
static double revDouble(const string &revNumStr) 
{
	string numStr = "";
    double number;
    const char *cNumStr;

	int len = (int)revNumStr.length();
	for(int i=len-1; i>=0 ; i--) {
		numStr += revNumStr[i];
	}

    cNumStr = numStr.c_str();
    
    number = strtod(cNumStr, NULL);
	return number;
}

/* function which returns the timezone offset corresponding to the argument epochsecs,
 *  which is the number of seconds since the epoch 
 */
int Literal::
findOffset(time_t epochsecs) 
{
	return timezone_offset( epochsecs, false );
} 


Literal* Literal::
MakeLiteral( const Value& val, Value::NumberFactor f ) 
{
	if(val.GetType()==Value::CLASSAD_VALUE || val.GetType()==Value::SCLASSAD_VALUE || val.GetType()==Value::LIST_VALUE || val.GetType()==Value::SLIST_VALUE){
		setError(ERR_BAD_VALUE, "list and classad values are not literals");
		return( NULL );
	}

	Literal* lit = new Literal();
	if( !lit ){
		setError(ERR_MEM_ALLOC_FAILED);
		return NULL;
	}
	lit->value.CopyFrom( val );
	if( !val.IsIntegerValue() && !val.IsRealValue() ) f = Value::NO_FACTOR;
	lit->value.factor = f;

	return lit;
}

bool Literal::
SameAs(const ExprTree *tree) const
{
    bool    is_same;
    const ExprTree * pSelfTree = tree->self();
    
    if (this == pSelfTree) {
        is_same = true;
    } else if (pSelfTree->GetKind() != LITERAL_NODE) {
        is_same = false;
    } else {
        const Literal *other_literal;
        
        other_literal = (const Literal *) pSelfTree;
        is_same = (   value.factor == other_literal->value.factor
                   && value.SameAs(other_literal->value));
    }
    return is_same;
}

bool 
operator==(Literal &literal1, Literal &literal2)
{
    return literal1.SameAs(&literal2);
}


bool Literal::
_Evaluate (EvalState &, Value &val) const
{
	val.CopyFrom( value );
	val.ApplyFactor();
	return( true );
}


bool Literal::
_Evaluate( EvalState &state, Value &val, ExprTree *&tree ) const
{
	_Evaluate( state, val );
	return( ( tree = Copy() ) );
}


bool Literal::
_Flatten( EvalState &state, Value &val, ExprTree *&tree, int*) const
{
	tree = NULL;
	return( _Evaluate( state, val ) );
}

static bool extractTimeZone(string &timeStr, int &tzhr, int &tzmin) 
{
	int    len    = (int)timeStr.length();
	int    i      = len-1; 
    bool   offset = false;
    string offStr = timeStr.substr(i-4,5);

    if (((offStr[0] == '+') || (offStr[0] == '-')) && 
        (isdigit(offStr[1])) && (isdigit(offStr[2])) && (isdigit(offStr[3])) && (isdigit(offStr[4]))) {
        offset = true;
        timeStr.erase(i-4,5);
        if (offStr[0] == '+') {
            tzhr = atoi(offStr.substr(1,2).c_str());
            tzmin = atoi(offStr.substr(3,2).c_str());
        }
        else {
            tzhr = -atoi(offStr.substr(1,2).c_str());
            tzmin = -atoi(offStr.substr(3,2).c_str());
        }
    }
    return offset;
}

} // classad
